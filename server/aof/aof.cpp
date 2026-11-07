#include "server/aof/aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "server/aof/file_ops.h"
#include "server/aof/manifest.h"
#include "server/aof/records.h"
#include "server/aof/snapshot.h"
#include "utils/check.h"

// The Aof's appends, flushing, and manifest updates. Startup and loading are
// in aof_load.cpp, and rewriting in aof_rewrite.cpp.
namespace redis_simple::aof {
namespace {
// Redis' AOF timing: `everysec` syncs once a second, a write waits at most
// two seconds for a running sync, and write errors are logged at most every
// 30 seconds.
constexpr std::chrono::seconds kSyncInterval{1};
constexpr std::chrono::seconds kMaxFlushPostpone{2};
constexpr std::chrono::seconds kWriteErrorLogInterval{30};
}  // namespace

std::string_view ErrorName(AofError error) {
  switch (error) {
    case AofError::kNone:
      return "none";
    case AofError::kWrite:
      return "write";
    case AofError::kSync:
      return "sync";
    case AofError::kRename:
      return "rename";
    case AofError::kDirectorySync:
      return "directory-sync";
    case AofError::kSnapshot:
      return "snapshot";
  }
  return "unknown";
}

std::unique_ptr<Aof> Aof::Open(const Options& options, db::RedisDb* const db) {
  if (db == nullptr || options.directory.empty() ||
      !ValidFileName(options.file_name) ||
      options.limits.max_replay_command_bytes == 0 ||
      options.limits.snapshot_block_bytes == 0) {
    return nullptr;
  }
  // Redis' startup: load the manifest (upgrading a single-file AOF), replay
  // the files, create a base file if there is none, open the last
  // incremental file, and delete history files.
  auto aof = std::unique_ptr<Aof>(new Aof(options, options.directory));
  if (!aof->LoadManifest() || !aof->Replay(db)) {
    return nullptr;
  }
  if (mkdir(options.directory.c_str(), 0755) < 0 && errno != EEXIST) {
    RS_LOG_ERROR("Can't open or create append-only dir %s: %s\n",
                 options.directory.c_str(), std::strerror(errno));
    return nullptr;
  }
  if (!aof->jobs_.Start()) {
    return nullptr;
  }
  if (!aof->CreateBaseIfMissing(db) || !aof->OpenLastIncrement()) {
    return nullptr;
  }
  aof->DeleteHistory();
  return aof;
}

Aof::Aof(const Options& options, std::string directory)
    : directory_(std::move(directory)),
      file_name_(options.file_name),
      fsync_(options.fsync),
      auto_rewrite_min_bytes_(options.auto_rewrite_min_bytes),
      auto_rewrite_percentage_(options.auto_rewrite_percentage),
      limits_(options.limits),
      load_truncated_(options.load_truncated),
      load_corrupt_tail_max_bytes_(options.load_corrupt_tail_max_bytes),
      timestamp_enabled_(options.timestamp_enabled),
      rewrite_incremental_fsync_(options.rewrite_incremental_fsync),
      no_fsync_on_rewrite_(options.no_fsync_on_rewrite),
      file_ops_(options.file_ops != nullptr ? options.file_ops
                                            : DefaultFileOps()),
      jobs_(file_ops_.get()),
      last_fsync_(std::chrono::steady_clock::now()) {}

Aof::~Aof() {
  // Redis' shutdown: kill a running rewrite, whether or not its child has
  // finished, then write and sync everything pending.
  if (rewrite_child_ > 0) {
    AbandonRewrite();
  }
  SyncNow();
  // Finishes the queued syncs before the current file closes.
  jobs_.Stop();
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool Aof::Append(std::string_view command,
                 const std::vector<std::string_view>& args,
                 db::RedisDb* const db) {
  if (timestamp_enabled_) {
    // Redis' genAofTimestampAnnotationIfNeeded: one annotation per second.
    const int64_t now = UnixSeconds();
    if (now > current_timestamp_) {
      current_timestamp_ = now;
      AppendTimestamp(now, &batch_);
    }
  }
  if (!database_selected_) {
    // Redis' feedAppendOnlyFile: a SELECT when the database differs from
    // the last one written, which no file has yet at startup or after a
    // rewrite starts.
    database_selected_ = true;
    batch_.append(kSelectRecord);
  }
  const size_t batch_size = batch_.size();
  if (!EncodeMutation(command, args, db, &batch_)) {
    // Drop any partially encoded record so the batch stays replayable.
    batch_.resize(batch_size);
    return false;
  }
  if (in_transaction_) {
    ++transaction_records_;
  }
  return true;
}

void Aof::BeginTransaction() {
  in_transaction_ = true;
  transaction_start_ = batch_.size();
  transaction_records_ = 0;
}

void Aof::EndTransaction() {
  if (!in_transaction_) {
    return;
  }
  in_transaction_ = false;
  if (transaction_records_ < 2) {
    return;
  }
  batch_.insert(transaction_start_, kMultiRecord);
  batch_.append(kExecRecord);
}

bool Aof::Flush(bool force) {
  // Nothing runs between an EXEC's commands, and BGREWRITEAOF inside EXEC
  // is scheduled, so a transaction's records are never split.
  RS_CHECK(!in_transaction_);
  LogSyncFailure();
  const auto now = std::chrono::steady_clock::now();
  bool sync_in_progress = false;
  if (batch_.empty()) {
    if (increment_fsync_offset_ == increment_size_) {
      return true;
    }
    // Written but not yet synced: with `everysec`, sync once a second even
    // when no new writes come; with `always`, after a policy change.
    if (fsync_ == FsyncPolicy::kEverySecond) {
      if (now - last_fsync_ < kSyncInterval || (jobs_.PendingFsyncs() != 0)) {
        return true;
      }
    } else if (fsync_ != FsyncPolicy::kAlways) {
      return true;
    }
  } else {
    if (fsync_ == FsyncPolicy::kEverySecond) {
      sync_in_progress = (jobs_.PendingFsyncs() != 0);
    }
    if (fsync_ == FsyncPolicy::kEverySecond && !force && sync_in_progress) {
      // On Linux a write blocks behind a running fsync of the same file, so
      // wait for it, but not for more than two seconds.
      if (!flush_postponed_start_.has_value()) {
        flush_postponed_start_ = now;
        return true;
      }
      if (now - *flush_postponed_start_ < kMaxFlushPostpone) {
        return true;
      }
      ++delayed_fsyncs_;
      RS_LOG_INFO(
          "Asynchronous AOF fsync is taking too long (disk is busy?). Writing "
          "the AOF buffer without waiting for fsync to complete, this may "
          "slow down the server.\n");
    }
    const ssize_t written = WriteAll(file_ops_.get(), fd_, batch_);
    flush_postponed_start_.reset();
    if (std::cmp_not_equal(written, batch_.size())) {
      FailWrite(written);
      // With `always` the replies already promise durability, so stop.
      return fsync_ != FsyncPolicy::kAlways;
    }
    if (write_failed_) {
      RS_LOG_INFO(
          "AOF write error looks solved, the server can write again.\n");
      write_failed_ = false;
    }
    current_size_ += batch_.size();
    increment_size_ += batch_.size();
    batch_.clear();
  }

  if (no_fsync_on_rewrite_ && rewrite_child_ > 0) {
    return true;
  }
  if (fsync_ == FsyncPolicy::kAlways) {
    if (!SyncFile(file_ops_.get(), fd_)) {
      RS_LOG_ERROR(
          "Can't persist AOF for fsync error when the AOF fsync policy is "
          "'always': %s. Exiting...\n",
          std::strerror(errno));
      return false;
    }
    increment_fsync_offset_ = increment_size_;
    last_fsync_ = now;
  } else if (fsync_ == FsyncPolicy::kEverySecond &&
             now - last_fsync_ >= kSyncInterval) {
    if (!sync_in_progress) {
      jobs_.QueueFsync(fd_, false);
      increment_fsync_offset_ = increment_size_;
    }
    last_fsync_ = now;
  }
  return true;
}

void Aof::FailWrite(ssize_t written) {
  const auto now = std::chrono::steady_clock::now();
  // Log at most once per 30 seconds, as Redis does.
  const bool can_log =
      now - last_write_error_log_ > kWriteErrorLogInterval ||
      last_write_error_log_ == std::chrono::steady_clock::time_point{};
  if (can_log) {
    last_write_error_log_ = now;
  }
  if (written < 0) {
    write_errno_ = errno;
    if (can_log) {
      RS_LOG_ERROR("Error writing to the AOF file: %s\n",
                   std::strerror(write_errno_));
    }
  } else {
    if (can_log) {
      RS_LOG_ERROR(
          "Short write while writing to the AOF file: (nwritten=%zd, "
          "expected=%zu)\n",
          written, batch_.size());
    }
    // Cut the partial write off, so the whole buffer is retried and a
    // crash meanwhile leaves no torn record.
    if (increment_size_ <=
            static_cast<uintmax_t>(std::numeric_limits<off_t>::max()) &&
        ftruncate(fd_, static_cast<off_t>(increment_size_)) == 0) {
      written = -1;
    } else if (can_log) {
      RS_LOG_ERROR(
          "Could not remove short write from the append-only file. The server "
          "may refuse to load the AOF the next time it starts. ftruncate: "
          "%s\n",
          std::strerror(errno));
    }
    write_errno_ = ENOSPC;
  }
  write_failed_ = true;
  if (written > 0) {
    // The written prefix stays in the file; keep only the rest.
    const auto size = static_cast<size_t>(written);
    current_size_ += size;
    increment_size_ += size;
    batch_.erase(0, size);
  }
}

bool Aof::AcceptingWrites() const {
  return !write_failed_ && !jobs_.FsyncFailed() && !manifest_unsafe_;
}

std::string Aof::LastErrorText() const {
  int error_number = write_errno_;
  if (!write_failed_ && jobs_.FsyncFailed()) {
    error_number = jobs_.FsyncErrno();
  } else if (!write_failed_ && manifest_unsafe_) {
    error_number = manifest_errno_;
  }
  return error_number != 0 ? std::strerror(error_number) : "unknown error";
}

bool Aof::SyncNow() {
  // Redis' shutdown: a forced flush, then an fsync whatever the policy.
  if (fd_ < 0) {
    return false;
  }
  const bool flushed = Flush(true) && batch_.empty();
  if (!SyncFile(file_ops_.get(), fd_)) {
    return false;
  }
  increment_fsync_offset_ = increment_size_;
  return flushed;
}

void Aof::WaitForBackgroundJobs() { jobs_.WaitUntilIdle(); }

void Aof::LogSyncFailure() {
  const bool failed = jobs_.FsyncFailed();
  if (failed && !fsync_failure_logged_) {
    RS_LOG_ERROR("Fail to fsync the AOF file: %s\n",
                 std::strerror(jobs_.FsyncErrno()));
  }
  fsync_failure_logged_ = failed;
}

void Aof::RecordError(AofError error, int error_number) {
  RS_LOG_ERROR("AOF %.*s failed: %s\n",
               static_cast<int>(ErrorName(error).size()),
               ErrorName(error).data(),
               error_number != 0 ? std::strerror(error_number) : "unknown");
}

AofState Aof::State() const {
  const auto now = std::chrono::steady_clock::now();
  return {
      .accepting_writes = AcceptingWrites(),
      .rewrite_in_progress = rewrite_child_ > 0,
      .rewrite_scheduled = rewrite_scheduled_,
      .last_rewrite_ok = last_rewrite_ok_,
      .last_rewrite_seconds = last_rewrite_seconds_,
      .current_rewrite_seconds =
          rewrite_child_ > 0 ? std::chrono::duration_cast<std::chrono::seconds>(
                                   now - rewrite_start_)
                                   .count()
                             : -1,
      .rewrites = rewrites_,
      .consecutive_rewrite_failures = consecutive_rewrite_failures_,
      .current_size = current_size_,
      .base_size = base_size_,
      .buffer_length = batch_.size(),
      .pending_fsyncs = jobs_.PendingFsyncs(),
      .delayed_fsyncs = delayed_fsyncs_,
  };
}

std::string Aof::FilePath(std::string_view name) const {
  std::string path = directory_;
  path.push_back('/');
  path.append(name);
  return path;
}

std::string Aof::ManifestPath() const {
  return FilePath(ManifestFileName(file_name_));
}

AofError Aof::PersistManifest(const Manifest& manifest) {
  const std::string temp_path =
      FilePath(std::string(kTempPrefix) + ManifestFileName(file_name_));
  const int fd = OpenFile(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    RS_LOG_ERROR("Can't open the temporary manifest file %s: %s\n",
                 temp_path.c_str(), std::strerror(errno));
    return AofError::kWrite;
  }
  const std::string text = manifest.Encode();
  int error_number = 0;
  const bool written =
      WritePrefix(file_ops_.get(), fd, text, &error_number) == text.size();
  const bool synced = written && SyncFile(file_ops_.get(), fd);
  if (!synced && error_number == 0) {
    error_number = errno;
  }
  close(fd);
  if (!written || !synced) {
    RS_LOG_ERROR("Error writing the temporary manifest file %s: %s\n",
                 temp_path.c_str(), std::strerror(error_number));
    unlink(temp_path.c_str());
    errno = error_number;
    return written ? AofError::kSync : AofError::kWrite;
  }
  if (file_ops_->Rename(temp_path.c_str(), ManifestPath().c_str()) < 0) {
    const int rename_errno = errno;
    RS_LOG_ERROR(
        "Error trying to rename the temporary manifest file %s into %s: %s\n",
        temp_path.c_str(), ManifestPath().c_str(), std::strerror(rename_errno));
    unlink(temp_path.c_str());
    errno = rename_errno;
    return AofError::kRename;
  }
  if (!SyncDirectory(file_ops_.get(), directory_)) {
    RS_LOG_ERROR("Fail to fsync manifest directory %s: %s.\n",
                 directory_.c_str(), std::strerror(errno));
    return AofError::kDirectorySync;
  }
  // Durable: a manifest a crash could have brought back is gone for good.
  manifest_unsafe_ = false;
  return AofError::kNone;
}

bool Aof::OpenNextIncrement() {
  // Appends go to a new file from here on (Redis' openNewIncrAofForAppend).
  Manifest manifest = manifest_;
  const int64_t sequence = ++manifest.increment_sequence;
  const std::string name = IncrementFileName(file_name_, sequence);
  const int fd =
      OpenFile(FilePath(name), O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    RS_LOG_ERROR("Can't open the append-only file %s: %s\n", name.c_str(),
                 std::strerror(errno));
    return false;
  }
  manifest.increments.push_back(
      MakeManifestFile(name, sequence, ManifestFile::Type::kIncrement));
  const AofError error = PersistManifest(manifest);
  const int error_number = errno;
  if (error != AofError::kNone && error != AofError::kDirectorySync) {
    close(fd);
    return false;
  }
  RS_LOG_INFO("Creating AOF incr file %s on background rewrite\n",
              name.c_str());
  // After the rename the new manifest is live, durable or not.
  manifest_ = std::move(manifest);
  SwitchFile(fd);
  if (error == AofError::kDirectorySync) {
    FailDirectorySync(error_number);
  }
  return true;
}

void Aof::SwitchFile(int fd) {
  // The background thread syncs and closes the replaced file, so the switch
  // does not wait for the disk.
  if (fd_ >= 0) {
    jobs_.QueueFsync(fd_, true);
    last_fsync_ = std::chrono::steady_clock::now();
  }
  fd_ = fd;
  increment_size_ = 0;
  increment_fsync_offset_ = 0;
}

void Aof::FailDirectorySync(int error_number) {
  // A rename that may not survive a crash. Redis treats it as an ordinary
  // failure and deletes the new files the live manifest lists; instead,
  // reject writes and keep the replaced files until a later manifest
  // persists safely.
  manifest_unsafe_ = true;
  manifest_errno_ = error_number;
  RecordError(AofError::kDirectorySync, error_number);
}

void Aof::DeleteHistory() {
  // Redis' aofDelHistoryFiles. After a manifest whose rename may not be
  // durable, a crash could bring back the previous manifest, which still
  // needs these files.
  if (manifest_.history.empty() || manifest_unsafe_) {
    return;
  }
  for (const ManifestFile& file : manifest_.history) {
    RS_LOG_INFO("Removing the history file %s in the background\n",
                file.name.c_str());
    jobs_.QueueDeletion(FilePath(file.name));
  }
  manifest_.history.clear();
  // A failure does not matter: history files are never loaded.
  PersistManifest(manifest_);
}

SnapshotOptions Aof::SnapshotSettings() const {
  return {.limits = limits_,
          .timestamp = timestamp_enabled_,
          .incremental_fsync = rewrite_incremental_fsync_};
}

}  // namespace redis_simple::aof
