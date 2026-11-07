#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "memory/dynamic_buffer.h"
#include "server/aof/aof.h"
#include "server/aof/file_ops.h"
#include "server/aof/manifest.h"
#include "server/aof/records.h"
#include "server/aof/snapshot.h"
#include "server/client.h"
#include "server/commands/command.h"
#include "server/db/db.h"
#include "server/request_parser.h"
#include "server/server_context.h"

// The Aof's startup: reading the manifest, upgrading a single-file AOF,
// replaying the files, and opening the file appends go to.
namespace redis_simple::aof {
namespace {
constexpr size_t kReadChunkSize = size_t{64} * 1024;
// The longest "#..." annotation line loading accepts (Redis'
// AOF_ANNOTATION_LINE_MAX_LEN).
constexpr size_t kMaxAnnotationBytes = 1024;

// Commands of one MULTI/EXEC block read during replay, held until EXEC.
class ReplayTransaction {
 public:
  void Queue(const command::Command* command,
             const command::CommandArgs& args) {
    commands_.push_back({command, args.size()});
    for (const std::string_view arg : args) {
      args_.emplace_back(arg);
    }
  }

  // Runs and clears the queued commands through `execute`, which returns
  // false when a command fails; Apply then stops and returns false.
  template <typename Execute>
  bool Apply(const Execute& execute) {
    command::CommandArgs views;
    size_t next_arg = 0;
    bool applied = true;
    for (const auto& [command, arg_count] : commands_) {
      views.assign(
          args_.begin() + static_cast<std::ptrdiff_t>(next_arg),
          args_.begin() + static_cast<std::ptrdiff_t>(next_arg + arg_count));
      next_arg += arg_count;
      if (!execute(command, &views)) {
        applied = false;
        break;
      }
    }
    commands_.clear();
    args_.clear();
    return applied;
  }

 private:
  struct QueuedCommand {
    const command::Command* command;
    size_t arg_count;
  };
  std::vector<QueuedCommand> commands_;
  std::vector<std::string> args_;
};

}  // namespace

bool Aof::LoadManifest() {
  // Redis' aofLoadManifestFromDisk: no directory or no manifest is an empty
  // AOF.
  const bool directory_exists = IsDirectory(directory_);
  if (directory_exists && PathExists(ManifestPath())) {
    const std::optional<std::string> text = ReadWholeFile(ManifestPath());
    if (!text.has_value()) {
      RS_LOG_ERROR("Fatal error: can't open the AOF manifest file %s\n",
                   ManifestPath().c_str());
      return false;
    }
    std::optional<Manifest> manifest = Manifest::Parse(*text);
    if (!manifest.has_value()) {
      RS_LOG_ERROR("Invalid AOF manifest file format: %s\n",
                   ManifestPath().c_str());
      return false;
    }
    manifest_ = std::move(*manifest);
  }
  // Redis' upgradeAofIfNeeded: a single-file AOF in the working directory
  // becomes the base file, when there is no AOF directory, when its manifest
  // lists no files, or when an earlier upgrade listed it but did not move
  // it yet.
  if (!PathExists(file_name_)) {
    return true;
  }
  const bool upgrade =
      !directory_exists ||
      (!manifest_.base.has_value() && manifest_.increments.empty()) ||
      (manifest_.base.has_value() && manifest_.increments.empty() &&
       manifest_.base->name == file_name_ && !PathExists(FilePath(file_name_)));
  if (!upgrade) {
    return true;
  }
  // Redis' aofUpgradePrepare: list the file first, then move it, so a crash
  // in between repeats the upgrade on the next start.
  if (mkdir(directory_.c_str(), 0755) < 0 && errno != EEXIST) {
    RS_LOG_ERROR("Can't open or create append-only dir %s: %s\n",
                 directory_.c_str(), std::strerror(errno));
    return false;
  }
  Manifest manifest = manifest_;
  manifest.base = MakeManifestFile(file_name_, 1, ManifestFile::Type::kBase);
  manifest.base_sequence = 1;
  if (PersistManifest(manifest) != AofError::kNone) {
    return false;
  }
  manifest_ = std::move(manifest);
  if (std::rename(file_name_.c_str(), FilePath(file_name_).c_str()) < 0) {
    RS_LOG_ERROR("Error trying to move the old AOF file %s into dir %s: %s\n",
                 file_name_.c_str(), directory_.c_str(), std::strerror(errno));
    return false;
  }
  RS_LOG_INFO(
      "Successfully migrated an old-style AOF file (%s) into the AOF "
      "directory (%s).\n",
      file_name_.c_str(), directory_.c_str());
  return true;
}

bool Aof::Replay(db::RedisDb* const db) {
  // Replays the base file, then each incremental file, as Redis'
  // loadAppendOnlyFiles does.
  std::vector<const ManifestFile*> files;
  if (manifest_.base.has_value()) {
    files.push_back(&*manifest_.base);
  }
  for (const ManifestFile& file : manifest_.increments) {
    files.push_back(&file);
  }
  // A listed file that is missing is fatal.
  size_t total_size = 0;
  for (const ManifestFile* const file : files) {
    const std::optional<size_t> size = PathSize(FilePath(file->name));
    if (!size.has_value()) {
      RS_LOG_ERROR("The append log file %s doesn't exist: %s\n",
                   file->name.c_str(), std::strerror(errno));
      return false;
    }
    total_size += *size;
  }
  if (total_size == 0) {
    return true;
  }
  for (size_t index = 0; index < files.size(); ++index) {
    const std::string path = FilePath(files[index]->name);
    const int fd = OpenFile(path, O_RDWR);
    if (fd < 0) {
      RS_LOG_ERROR(
          "Fatal error: can't open the append log file %s for reading: %s\n",
          files[index]->name.c_str(), std::strerror(errno));
      return false;
    }
    if (files[index]->type == ManifestFile::Type::kBase) {
      base_size_ = FileSize(fd).value_or(0);
    }
    // Only the last file may end in a torn record.
    const bool replayed = ReplayFile(db, fd, index + 1 == files.size());
    close(fd);
    if (!replayed) {
      RS_LOG_ERROR("cannot load AOF file %s\n", path.c_str());
      return false;
    }
  }
  current_size_ = total_size;
  return true;
}

bool Aof::ReplayFile(db::RedisDb* const db, int fd, bool last) {
  db->SetLoading(true);
  // Where a format error starts the corrupt tail: after the last complete
  // command, and before a transaction it interrupts.
  std::optional<size_t> corrupt_from;
  bool replayed = [this, db, fd, last, &corrupt_from] {
    const ServerContext server{
        .db = db, .aof = nullptr, .required_password = {}, .io_threads = {}};
    Client client(&server);
    std::array<char, kReadChunkSize> chunk{};
    in_memory::DynamicBuffer input;
    command::CommandArgs args;
    size_t complete_bytes = 0;
    bool reached_end = false;
    // Records between MULTI and EXEC are copied out of `input` and applied
    // together at EXEC, so a transaction cut off by a crash is not applied.
    std::optional<size_t> transaction_start;
    ReplayTransaction transaction;

    while (!reached_end) {
      ssize_t bytes_read = -1;
      while (true) {
        bytes_read = read(fd, chunk.data(), chunk.size());
        if (bytes_read >= 0 || errno != EINTR) {
          break;
        }
      }
      if (bytes_read < 0) {
        return false;
      }
      reached_end = bytes_read == 0;
      if (bytes_read > 0) {
        input.Append(chunk.data(), static_cast<size_t>(bytes_read));
      }

      while (!input.Empty()) {
        const std::string_view remaining = input.View();
        if (remaining.front() == '#') {
          // An annotation, such as "#TS:<unix time>"; Redis skips it.
          const size_t line_end = remaining.find('\n');
          if (line_end == std::string_view::npos && !reached_end) {
            if (remaining.size() > kMaxAnnotationBytes) {
              corrupt_from = transaction_start.value_or(complete_bytes);
              return false;
            }
            break;
          }
          const size_t line_size = line_end == std::string_view::npos
                                       ? remaining.size()
                                       : line_end + 1;
          if (!AddSize(line_size, &complete_bytes)) {
            return false;
          }
          input.Consume(line_size);
          continue;
        }
        if (remaining.front() != '*') {
          corrupt_from = transaction_start.value_or(complete_bytes);
          return false;
        }
        std::string_view name;
        const request_parser::ParseResult parsed =
            request_parser::Parse(remaining, &name, &args);
        if (parsed.status == request_parser::ParseStatus::kIncomplete) {
          break;
        }
        if (parsed.status != request_parser::ParseStatus::kComplete) {
          corrupt_from = transaction_start.value_or(complete_bytes);
          return false;
        }
        if (parsed.consumed > limits_.max_replay_command_bytes) {
          return false;
        }
        if (name == "SELECT" && args.size() == 1) {
          // The only database there is; any other would lose data.
          if (args[0] != "0") {
            RS_LOG_ERROR("AOF selects database %.*s, but only 0 exists\n",
                         static_cast<int>(args[0].size()), args[0].data());
            return false;
          }
        } else if (name == "MULTI" && args.empty()) {
          if (transaction_start.has_value()) {
            return false;
          }
          transaction_start = complete_bytes;
        } else if (name == "EXEC" && args.empty()) {
          const auto execute = [&client](const command::Command* command,
                                         command::CommandArgs* views) {
            return client.ExecuteForReplay(command, views);
          };
          if (!transaction_start.has_value() || !transaction.Apply(execute)) {
            return false;
          }
          transaction_start.reset();
        } else {
          const command::Command* metadata = command::Find(name);
          if (metadata == nullptr ||
              metadata->access != command::CommandAccess::kWrite ||
              !metadata->arity.Accepts(args.size())) {
            return false;
          }
          if (transaction_start.has_value()) {
            transaction.Queue(metadata, args);
          } else if (!client.ExecuteForReplay(metadata, &args)) {
            return false;
          }
        }
        if (!AddSize(parsed.consumed, &complete_bytes)) {
          return false;
        }
        input.Consume(parsed.consumed);
      }
      if (input.View().size() > limits_.max_replay_command_bytes) {
        return false;
      }
    }

    // Redis' unexpected end of file: a torn record, or a transaction without
    // its EXEC.
    const bool truncated = !input.Empty() || transaction_start.has_value();
    if (!truncated) {
      return true;
    }
    const size_t valid_bytes = transaction_start.value_or(complete_bytes);
    if (!load_truncated_) {
      RS_LOG_ERROR(
          "Unexpected end of file reading the append only file. You can set "
          "the 'aof-load-truncated' configuration option to yes and restart "
          "the server.\n");
      return false;
    }
    if (!last) {
      RS_LOG_ERROR("Fatal error: the truncated file is not the last file\n");
      return false;
    }
    RS_LOG_WARN("!!! Truncating the AOF at offset %zu !!!\n", valid_bytes);
    return CutTail(fd, valid_bytes);
  }();
  if (!replayed && corrupt_from.has_value()) {
    // Redis' aof-load-corrupt-tail-max-size: a format error with fewer than
    // that many bytes after the last complete command, in the last file.
    const size_t size = FileSize(fd).value_or(0);
    const size_t remaining = size - std::min(size, *corrupt_from);
    if (load_corrupt_tail_max_bytes_ > 0 &&
        remaining < load_corrupt_tail_max_bytes_) {
      if (!last) {
        RS_LOG_ERROR("Fatal error: the truncated file is not the last file\n");
      } else {
        RS_LOG_WARN(
            "!!! Warning: corrupt AOF file tail!!! Truncating the AOF at "
            "offset %zu (remaining %zu) !!!\n",
            *corrupt_from, remaining);
        replayed = CutTail(fd, *corrupt_from);
      }
    } else {
      RS_LOG_ERROR(
          "Bad file format reading the append only file at offset %zu. You "
          "can set the 'aof-load-corrupt-tail-max-size' configuration option "
          "to %zu and restart the server.\n",
          *corrupt_from, remaining);
    }
  }
  db->SetLoading(false);
  return replayed;
}

bool Aof::CutTail(int fd, size_t valid_bytes) const {
  // Cuts a torn or corrupt tail off the file, then syncs it (Redis'
  // truncateAppendOnlyFile).
  if (valid_bytes > static_cast<uintmax_t>(std::numeric_limits<off_t>::max()) ||
      ftruncate(fd, static_cast<off_t>(valid_bytes)) < 0) {
    RS_LOG_ERROR("Error truncating the AOF file: %s\n", std::strerror(errno));
    return false;
  }
  return SyncFile(file_ops_.get(), fd);
}

bool Aof::CreateBaseIfMissing(db::RedisDb* const db) {
  // Redis' aofOpenIfNeededOnServerStart: an AOF with no files starts with a
  // base file holding the current dataset.
  if (manifest_.base.has_value() || !manifest_.increments.empty()) {
    return true;
  }
  const int64_t sequence = manifest_.base_sequence + 1;
  const std::string name = BaseFileName(file_name_, sequence);
  const std::string temp_path =
      FilePath(std::string(kTempPrefix) + "rewriteaof-" +
               std::to_string(getpid()) + ".aof");
  if (WriteSnapshotFile(db, temp_path, SnapshotSettings(), file_ops_.get()) !=
      SnapshotStatus::kWritten) {
    RS_LOG_ERROR("Write error writing append only file on disk: %s\n",
                 std::strerror(errno));
    return false;
  }
  if (std::rename(temp_path.c_str(), FilePath(name).c_str()) < 0) {
    RS_LOG_ERROR(
        "Error moving temp append only file on the final destination: %s\n",
        std::strerror(errno));
    unlink(temp_path.c_str());
    return false;
  }
  manifest_.base = MakeManifestFile(name, sequence, ManifestFile::Type::kBase);
  manifest_.base_sequence = sequence;
  RS_LOG_INFO("Creating AOF base file %s on server start\n", name.c_str());
  return true;
}

bool Aof::OpenLastIncrement() {
  // Appends continue in the last incremental file, as in Redis; only a
  // rewrite starts a new one.
  Manifest manifest = manifest_;
  const bool created = manifest.increments.empty();
  if (created) {
    const int64_t sequence = ++manifest.increment_sequence;
    manifest.increments.push_back(
        MakeManifestFile(IncrementFileName(file_name_, sequence), sequence,
                         ManifestFile::Type::kIncrement));
  }
  const std::string& name = manifest.increments.back().name;
  const int fd = OpenFile(FilePath(name), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) {
    RS_LOG_ERROR("Can't open the append-only file %s: %s\n", name.c_str(),
                 std::strerror(errno));
    return false;
  }
  // A new incremental file, and a base CreateBaseIfMissing just made, which
  // only happens along with one, still need listing.
  if (created && PersistManifest(manifest) != AofError::kNone) {
    close(fd);
    return false;
  }
  manifest_ = std::move(manifest);
  fd_ = fd;
  increment_size_ = FileSize(fd).value_or(0);
  increment_fsync_offset_ = increment_size_;
  RS_LOG_INFO("%s AOF incr file %s on server start\n",
              created ? "Creating" : "Opening", name.c_str());
  return true;
}
}  // namespace redis_simple::aof
