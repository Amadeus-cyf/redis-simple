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
#include "server/rdb/rdb_loader.h"
#include "server/request_parser.h"
#include "server/server_context.h"
#include "utils/time_utils.h"

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
  void Queue(const commands::Command* command,
             const commands::CommandArgs& args) {
    commands_.push_back({command, args.size()});
    for (const std::string_view argument : args) {
      args_.emplace_back(argument);
    }
  }

  // Runs and clears the queued commands through `execute`, which returns
  // false when a command fails; Apply then stops and returns false.
  template <typename Execute>
  bool Apply(const Execute& execute) {
    commands::CommandArgs views;
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
    const commands::Command* command;
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
    const bool replayed =
        ReplayFile(db, fd, files[index]->name, index + 1 == files.size());
    close(fd);
    if (!replayed) {
      RS_LOG_ERROR("cannot load AOF file %s\n", path.c_str());
      return false;
    }
  }
  current_size_ = total_size;
  return true;
}

// Replays one AOF file into a database through a client without a
// connection, as Redis' loadSingleAppendOnlyFile does.
class FileReplayer {
 public:
  // `start_bytes` is where the commands start in the file: after an RDB
  // preamble, if there is one, so offsets count from the file's start.
  FileReplayer(db::RedisDb* const db, const Limits& limits, size_t start_bytes)
      : server_{.db = db,
                .aof = nullptr,
                .required_password = {},
                .io_threads = {}},
        client_(&server_),
        limits_(&limits),
        complete_bytes_(start_bytes) {}

  enum class Outcome : uint8_t {
    // Every record applied.
    kComplete,
    // The file ends in a torn record or a transaction without its EXEC.
    kTruncated,
    // A record is not in the command format; see CorruptFrom().
    kCorrupt,
    // A read failed, or a command could not be applied.
    kFailed,
  };

  Outcome Replay(int fd);

  // Changes the replayed commands made (Client::MarkModified).
  [[nodiscard]] int64_t Changes() const { return client_.changes_; }

  // Where the valid prefix ends: after the last complete command, and
  // before any transaction left open.
  [[nodiscard]] size_t ValidBytes() const {
    return transaction_start_.value_or(complete_bytes_);
  }

 private:
  enum class Step : uint8_t { kApplied, kNeedMoreInput, kCorrupt, kFailed };

  Step ApplyNext(bool reached_end);
  Step SkipAnnotation(std::string_view remaining, bool reached_end);
  bool ApplyCommand(std::string_view name);

  const ServerContext server_;
  Client client_;
  const Limits* const limits_;
  memory::DynamicBuffer input_;
  commands::CommandArgs args_;
  size_t complete_bytes_{};
  // Records between MULTI and EXEC are copied out of `input_` and applied
  // together at EXEC, so a transaction cut off by a crash is not applied.
  std::optional<size_t> transaction_start_;
  ReplayTransaction transaction_;
};

FileReplayer::Outcome FileReplayer::Replay(int fd) {
  std::array<char, kReadChunkSize> chunk{};
  bool reached_end = false;
  while (!reached_end) {
    ssize_t bytes_read = read(fd, chunk.data(), chunk.size());
    while (bytes_read < 0 && errno == EINTR) {
      bytes_read = read(fd, chunk.data(), chunk.size());
    }
    if (bytes_read < 0) {
      return Outcome::kFailed;
    }
    reached_end = bytes_read == 0;
    if (bytes_read > 0) {
      input_.Append(chunk.data(), static_cast<size_t>(bytes_read));
    }
    Step step = Step::kApplied;
    while (!input_.Empty() && step == Step::kApplied) {
      step = ApplyNext(reached_end);
    }
    if (step == Step::kCorrupt) {
      return Outcome::kCorrupt;
    }
    if (step == Step::kFailed ||
        input_.View().size() > limits_->max_replay_command_bytes) {
      return Outcome::kFailed;
    }
  }
  return input_.Empty() && !transaction_start_.has_value()
             ? Outcome::kComplete
             : Outcome::kTruncated;
}

FileReplayer::Step FileReplayer::ApplyNext(bool reached_end) {
  const std::string_view remaining = input_.View();
  if (remaining.front() == '#') {
    return SkipAnnotation(remaining, reached_end);
  }
  if (remaining.front() != '*') {
    return Step::kCorrupt;
  }
  std::string_view name;
  const request_parser::ParseResult parsed =
      request_parser::Parse(remaining, &name, &args_);
  if (parsed.status == request_parser::ParseStatus::kIncomplete) {
    return Step::kNeedMoreInput;
  }
  if (parsed.status != request_parser::ParseStatus::kComplete) {
    return Step::kCorrupt;
  }
  if (parsed.consumed > limits_->max_replay_command_bytes ||
      !ApplyCommand(name) || !AddSize(parsed.consumed, &complete_bytes_)) {
    return Step::kFailed;
  }
  input_.Consume(parsed.consumed);
  return Step::kApplied;
}

// Skips an annotation line, such as "#TS:<unix time>", as Redis does.
FileReplayer::Step FileReplayer::SkipAnnotation(std::string_view remaining,
                                                bool reached_end) {
  const size_t line_end = remaining.find('\n');
  if (line_end == std::string_view::npos && !reached_end) {
    return remaining.size() > kMaxAnnotationBytes ? Step::kCorrupt
                                                  : Step::kNeedMoreInput;
  }
  const size_t line_size =
      line_end == std::string_view::npos ? remaining.size() : line_end + 1;
  if (!AddSize(line_size, &complete_bytes_)) {
    return Step::kFailed;
  }
  input_.Consume(line_size);
  return Step::kApplied;
}

bool FileReplayer::ApplyCommand(std::string_view name) {
  if (name == "SELECT" && args_.size() == 1) {
    // The only database there is; any other would lose data.
    if (args_[0] != "0") {
      RS_LOG_ERROR("AOF selects database %.*s, but only 0 exists\n",
                   static_cast<int>(args_[0].size()), args_[0].data());
      return false;
    }
    return true;
  }
  if (name == "MULTI" && args_.empty()) {
    if (transaction_start_.has_value()) {
      return false;
    }
    transaction_start_ = complete_bytes_;
    return true;
  }
  if (name == "EXEC" && args_.empty()) {
    if (!transaction_start_.has_value() ||
        !transaction_.Apply([this](const commands::Command* command,
                                   commands::CommandArgs* views) {
          return client_.ExecuteForReplay(command, views);
        })) {
      return false;
    }
    transaction_start_.reset();
    return true;
  }
  const commands::Command* const command = commands::Find(name);
  if (command == nullptr ||
      command->access != commands::CommandAccess::kWrite ||
      !command->arity.Accepts(args_.size())) {
    return false;
  }
  if (transaction_start_.has_value()) {
    transaction_.Queue(command, args_);
    return true;
  }
  return client_.ExecuteForReplay(command, &args_);
}

bool Aof::LoadRdbPreamble(db::RedisDb* const db, int fd,
                          const std::string& name,
                          size_t* const preamble_bytes) const {
  // Redis reads the base file, or an old single file, as RDB when it starts
  // with "REDIS", whatever aof-use-rdb-preamble says, and then any commands
  // after it.
  std::array<char, 5> signature{};
  if (pread(fd, signature.data(), signature.size(), 0) !=
          static_cast<ssize_t>(signature.size()) ||
      std::string_view(signature.data(), signature.size()) != "REDIS") {
    return true;
  }
  // An old single file keeps the configured name; a base file has its own.
  const bool old_style = name == file_name_;
  RS_LOG_INFO(old_style ? "Reading RDB preamble from AOF file...\n"
                        : "Reading RDB base file on AOF loading...\n");
  rdb::LoadStats stats;
  // Redis' RDBFLAGS_AOF_PREAMBLE: keys whose time has passed are kept, as
  // the commands after them were written with those keys present; they
  // expire once loading is done.
  if (!rdb::LoadRdb(
          fd, db, {.now_ms = utils::NowInMilliseconds(), .keep_expired = true},
          &stats)) {
    RS_LOG_ERROR(old_style ? "Error reading the RDB preamble of the AOF file "
                             "%s, AOF loading aborted\n"
                           : "Error reading the RDB base file %s, AOF loading "
                             "aborted\n",
                 name.c_str());
    return false;
  }
  if (lseek(fd, static_cast<off_t>(stats.bytes), SEEK_SET) < 0) {
    RS_LOG_ERROR("Unrecoverable error reading the append only file %s: %s\n",
                 name.c_str(), std::strerror(errno));
    return false;
  }
  if (old_style) {
    RS_LOG_INFO("Reading the remaining AOF tail...\n");
  }
  *preamble_bytes = stats.bytes;
  return true;
}

bool Aof::ReplayFile(db::RedisDb* const db, int fd, const std::string& name,
                     bool last) {
  size_t preamble_bytes = 0;
  if (!LoadRdbPreamble(db, fd, name, &preamble_bytes)) {
    return false;
  }
  db->SetLoading(true);
  FileReplayer replayer(db, limits_, preamble_bytes);
  bool replayed = false;
  switch (replayer.Replay(fd)) {
    case FileReplayer::Outcome::kComplete:
      replayed = true;
      break;
    case FileReplayer::Outcome::kTruncated:
      replayed = HandleTruncatedFile(fd, last, replayer.ValidBytes());
      break;
    case FileReplayer::Outcome::kCorrupt:
      replayed = HandleCorruptTail(fd, last, replayer.ValidBytes());
      break;
    case FileReplayer::Outcome::kFailed:
      break;
  }
  replayed_changes_ += replayer.Changes();
  db->SetLoading(false);
  return replayed;
}

// Redis' "unexpected end of file": a torn record, or a transaction without
// its EXEC. With aof-load-truncated, the last file is cut back to its valid
// prefix.
bool Aof::HandleTruncatedFile(int fd, bool last, size_t valid_bytes) const {
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
}

// Redis' aof-load-corrupt-tail-max-size: a format error with fewer than that
// many bytes after the last complete command, in the last file, is cut off.
bool Aof::HandleCorruptTail(int fd, bool last, size_t corrupt_from) const {
  const size_t size = FileSize(fd).value_or(0);
  const size_t remaining = size - std::min(size, corrupt_from);
  if (load_corrupt_tail_max_bytes_ == 0 ||
      remaining >= load_corrupt_tail_max_bytes_) {
    RS_LOG_ERROR(
        "Bad file format reading the append only file at offset %zu. You "
        "can set the 'aof-load-corrupt-tail-max-size' configuration option "
        "to %zu and restart the server.\n",
        corrupt_from, remaining);
    return false;
  }
  if (!last) {
    RS_LOG_ERROR("Fatal error: the truncated file is not the last file\n");
    return false;
  }
  RS_LOG_WARN(
      "!!! Warning: corrupt AOF file tail!!! Truncating the AOF at offset %zu "
      "(remaining %zu) !!!\n",
      corrupt_from, remaining);
  return CutTail(fd, corrupt_from);
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
  const std::string name =
      BaseFileName(file_name_, sequence, use_rdb_preamble_);
  const std::string temp_path =
      FilePath(std::string(kTempPrefix) + "rewriteaof-" +
               std::to_string(getpid()) + ".aof");
  if (WriteBaseFile(db, temp_path) != SnapshotStatus::kWritten) {
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
