#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "server/aof/aof.h"
#include "server/aof/file_ops.h"
#include "server/aof/manifest.h"
#include "server/aof/snapshot.h"
#include "server/rdb/rdb_writer.h"
#include "utils/check.h"
#include "utils/time.h"

// The Aof's rewrite: a forked child writes a snapshot as the next base file,
// and the parent installs it with a new manifest once the child exits.
namespace redis_simple::aof {
namespace {
// After this many failed rewrites in a row, automatic rewrites back off, up
// to an hour apart (Redis' AOF_REWRITE_LIMITE_THRESHOLD and
// AOF_REWRITE_LIMITE_MAX_MINUTES).
constexpr size_t kFailuresBeforeBackoff = 3;
constexpr int64_t kMaxBackoffMinutes = 60;

pid_t WaitForChild(pid_t child, int* const wait_status, int options) {
  while (true) {
    const pid_t result = waitpid(child, wait_status, options);
    if (result >= 0 || errno != EINTR) {
      return result;
    }
  }
}

AofError ChildError(int wait_status) {
  if (!WIFEXITED(wait_status)) {
    return AofError::kSnapshot;
  }
  switch (WEXITSTATUS(wait_status)) {
    case static_cast<int>(SnapshotStatus::kWriteFailed):
      return AofError::kWrite;
    case static_cast<int>(SnapshotStatus::kSyncFailed):
      return AofError::kSync;
    default:
      return AofError::kSnapshot;
  }
}
}  // namespace

void Aof::ScheduleRewrite() {
  rewrite_scheduled_ = true;
  // A manual rewrite is not held back by earlier failures.
  consecutive_rewrite_failures_ = 0;
}

RewriteResult Aof::StartRewrite(db::RedisDb* const db) {
  if (db == nullptr) {
    return RewriteResult::kError;
  }
  if (rewrite_child_ > 0) {
    return RewriteResult::kInProgress;
  }
  if (mkdir(directory_.c_str(), 0755) < 0 && errno != EEXIST) {
    RS_LOG_ERROR("Can't open or create append-only dir %s: %s\n",
                 directory_.c_str(), std::strerror(errno));
    last_rewrite_ok_ = false;
    return RewriteResult::kError;
  }
  // Every mutation applied to `db` must be in the current incremental file
  // before the switch: the snapshot covers them, and the next file must
  // not. Redis carries bytes a failed write left in the buffer into the new
  // file, where replay would apply them twice; wait them out instead.
  Flush(true);
  if (!batch_.empty()) {
    last_rewrite_ok_ = false;
    return RewriteResult::kError;
  }
  // Redis resets aof_selected_db before switching files, so the new file
  // starts with a SELECT.
  database_selected_ = false;
  if (!OpenNextIncrement()) {
    last_rewrite_ok_ = false;
    return RewriteResult::kError;
  }
  ++rewrites_;
  if (before_fork_) {
    before_fork_();
  }
  const pid_t child = fork();
  if (child == 0) {
    RunRewriteChild(db);
  }
  const int fork_errno = errno;
  if (after_fork_) {
    after_fork_();
  }
  if (child < 0) {
    last_rewrite_ok_ = false;
    RS_LOG_ERROR("Can't rewrite append only file in background: fork: %s\n",
                 std::strerror(fork_errno));
    return RewriteResult::kError;
  }
  RS_LOG_INFO("Background append only file rewriting started by pid %d\n",
              static_cast<int>(child));
  rewrite_scheduled_ = false;
  rewrite_child_ = child;
  rewrite_start_ = std::chrono::steady_clock::now();
  return RewriteResult::kStarted;
}

SnapshotStatus Aof::WriteBaseFile(db::RedisDb* const db,
                                  const std::string& path) const {
  if (!use_rdb_preamble_) {
    return WriteSnapshotFile(db, path, SnapshotSettings(), file_ops_.get());
  }
  // Redis' rewriteAppendOnlyFile with aof-use-rdb-preamble: an RDB file
  // marked with the "aof-base" aux field.
  switch (
      rdb::WriteRdbFile(db, path,
                        {.compression = rdb_compression_,
                         .checksum = rdb_checksum_,
                         .incremental_fsync = rewrite_incremental_fsync_,
                         .aof_base = true,
                         .creation_time = utils::NowInMilliseconds() / 1000},
                        file_ops_.get())) {
    case rdb::WriteStatus::kWritten:
      return SnapshotStatus::kWritten;
    case rdb::WriteStatus::kWriteFailed:
      return SnapshotStatus::kWriteFailed;
    case rdb::WriteStatus::kSyncFailed:
      return SnapshotStatus::kSyncFailed;
  }
  utils::Unreachable();
}

void Aof::RunRewriteChild(db::RedisDb* const db) const {
  // The child writes temp-rewriteaof-<pid>.aof and renames it to
  // temp-rewriteaof-bg-<pid>.aof once it is complete and synced.
  const pid_t self = getpid();
  const std::string path = FilePath(std::string(kTempPrefix) + "rewriteaof-" +
                                    std::to_string(self) + ".aof");
  SnapshotStatus status = WriteBaseFile(db, path);
  if (status == SnapshotStatus::kWritten &&
      std::rename(path.c_str(), RewriteTempPath(self).c_str()) < 0) {
    unlink(path.c_str());
    status = SnapshotStatus::kWriteFailed;
  }
  _exit(static_cast<int>(status));
}

std::string Aof::RewriteTempPath(pid_t child) const {
  // Redis' temp-rewriteaof-bg-<pid>.aof, in the AOF directory rather than
  // Redis' working directory, since --appenddirname may name another file
  // system.
  return FilePath(std::string(kTempPrefix) + "rewriteaof-bg-" +
                  std::to_string(child) + ".aof");
}

void Aof::CheckRewrite() {
  if (rewrite_child_ <= 0) {
    return;
  }
  int wait_status = 0;
  const pid_t result = WaitForChild(rewrite_child_, &wait_status, WNOHANG);
  if (result != 0) {
    FinishRewrite(result < 0 ? -1 : wait_status);
  }
}

void Aof::WaitForRewrite() {
  if (rewrite_child_ <= 0) {
    return;
  }
  int wait_status = 0;
  const pid_t result = WaitForChild(rewrite_child_, &wait_status, 0);
  FinishRewrite(result < 0 ? -1 : wait_status);
}

void Aof::FinishRewrite(int wait_status) {
  // Installs the child's snapshot if it succeeded, and records the outcome
  // (Redis' backgroundRewriteDoneHandler).
  const pid_t child = rewrite_child_;
  rewrite_child_ = -1;
  if (wait_status != -1 && WIFEXITED(wait_status) &&
      WEXITSTATUS(wait_status) == static_cast<int>(SnapshotStatus::kWritten)) {
    RS_LOG_INFO("Background AOF rewrite terminated with success\n");
    const Failure failure = InstallRewrite(child);
    if (failure.Ok()) {
      last_rewrite_ok_ = true;
      consecutive_rewrite_failures_ = 0;
      RS_LOG_INFO("Background AOF rewrite finished successfully\n");
    } else {
      RecordError(failure.error, failure.error_number);
      last_rewrite_ok_ = false;
      ++consecutive_rewrite_failures_;
    }
  } else if (wait_status != -1 && WIFSIGNALED(wait_status)) {
    RS_LOG_WARN("Background AOF rewrite terminated by signal %d\n",
                WTERMSIG(wait_status));
    // SIGUSR1 is how a rewrite is stopped on purpose, not a failure.
    if (WTERMSIG(wait_status) != SIGUSR1) {
      last_rewrite_ok_ = false;
      ++consecutive_rewrite_failures_;
    }
  } else {
    RecordError(
        wait_status == -1 ? AofError::kSnapshot : ChildError(wait_status), 0);
    RS_LOG_WARN("Background AOF rewrite terminated with error\n");
    last_rewrite_ok_ = false;
    ++consecutive_rewrite_failures_;
  }
  RemoveRewriteTempFiles(child);
  last_rewrite_seconds_ = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - rewrite_start_)
                              .count();
}

Aof::Failure Aof::InstallRewrite(pid_t child) {
  // The previous base and every incremental file but the current one become
  // history, in Redis' order: the incremental files first, then the base.
  Manifest manifest = manifest_;
  if (manifest.base.has_value()) {
    manifest.base->type = ManifestFile::Type::kHistory;
    manifest.history.insert(manifest.history.begin(),
                            std::move(*manifest.base));
  }
  const int64_t base_sequence = ++manifest.base_sequence;
  const std::string base_name =
      BaseFileName(file_name_, base_sequence, use_rdb_preamble_);
  manifest.base =
      MakeManifestFile(base_name, base_sequence, ManifestFile::Type::kBase);
  if (file_ops_->Rename(RewriteTempPath(child).c_str(),
                        FilePath(base_name).c_str()) < 0) {
    const int rename_errno = errno;
    RS_LOG_ERROR(
        "Error trying to rename the temporary AOF base file into %s: %s\n",
        base_name.c_str(), std::strerror(rename_errno));
    return {AofError::kRename, rename_errno};
  }
  if (manifest.increments.size() > 1) {
    std::vector<ManifestFile> rewritten(manifest.increments.begin(),
                                        manifest.increments.end() - 1);
    for (ManifestFile& file : rewritten) {
      file.type = ManifestFile::Type::kHistory;
    }
    manifest.history.insert(manifest.history.begin(),
                            std::make_move_iterator(rewritten.begin()),
                            std::make_move_iterator(rewritten.end()));
    manifest.increments.erase(manifest.increments.begin(),
                              manifest.increments.end() - 1);
  }
  const Failure failure = PersistManifest(manifest);
  if (!failure.Ok() && failure.error != AofError::kDirectorySync) {
    // The old manifest still describes the AOF; drop the unused base.
    jobs_.QueueDeletion(FilePath(base_name));
    return failure;
  }
  // After the rename the new manifest is live, durable or not.
  manifest_ = std::move(manifest);
  current_size_ = PathSize(FilePath(base_name)).value_or(0) + increment_size_;
  base_size_ = current_size_;
  if (!failure.Ok()) {
    FailDirectorySync(failure.error_number);
    return failure;
  }
  DeleteHistory();
  return {};
}

void Aof::RemoveRewriteTempFiles(pid_t child) {
  // Deletes whatever the child left behind (Redis' aofRemoveTempFile).
  jobs_.QueueDeletion(RewriteTempPath(child));
  jobs_.QueueDeletion(FilePath(std::string(kTempPrefix) + "rewriteaof-" +
                               std::to_string(child) + ".aof"));
}

void Aof::AbandonRewrite() {
  // Redis' killAppendOnlyChild: stop the child, finished or not, and leave
  // the rewrite statistics as they were.
  RS_LOG_INFO("Killing running AOF rewrite child: %d\n",
              static_cast<int>(rewrite_child_));
  if (kill(rewrite_child_, SIGUSR1) == 0) {
    int wait_status = 0;
    WaitForChild(rewrite_child_, &wait_status, 0);
  }
  RemoveRewriteTempFiles(rewrite_child_);
  rewrite_child_ = -1;
}

bool Aof::RewriteLimited() {
  // Redis' aofRewriteLimited: after three consecutive failures, automatic
  // and scheduled rewrites wait a minute, then two, four, and so on up to an
  // hour. BGREWRITEAOF still starts one at once.
  if (consecutive_rewrite_failures_ < kFailuresBeforeBackoff) {
    next_delay_minutes_ = 0;
    next_rewrite_time_.reset();
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (next_rewrite_time_.has_value()) {
    if (now < *next_rewrite_time_) {
      return true;
    }
    next_rewrite_time_.reset();
    return false;
  }
  next_delay_minutes_ =
      next_delay_minutes_ == 0
          ? 1
          : std::min(next_delay_minutes_ * 2, kMaxBackoffMinutes);
  next_rewrite_time_ = now + std::chrono::minutes(next_delay_minutes_);
  RS_LOG_WARN(
      "Background AOF rewrite has repeatedly failed and triggered the limit, "
      "will retry in %" PRId64 " minutes\n",
      next_delay_minutes_);
  return true;
}

bool Aof::ShouldAutoRewrite() {
  // Redis' serverCron condition, with its integer growth percentage.
  if (rewrite_child_ > 0 || auto_rewrite_percentage_ == 0 ||
      current_size_ <= auto_rewrite_min_bytes_) {
    return false;
  }
  const uint64_t base = base_size_ != 0 ? base_size_ : 1;
  const auto current = static_cast<uint64_t>(current_size_);
  // current * 100 / base - 100, without overflow.
  const uint64_t ratio =
      ((current / base) * 100) + (((current % base) * 100) / base);
  if (ratio < 100 || ratio - 100 < auto_rewrite_percentage_) {
    return false;
  }
  return !RewriteLimited();
}

bool Aof::Cron(db::RedisDb* const db, bool may_fork) {
  other_child_running_ = !may_fork;
  // A write postponed behind a sync, or a failed one, is retried here too.
  if (!Flush()) {
    return false;
  }
  if (may_fork && rewrite_child_ <= 0 && rewrite_scheduled_ &&
      !RewriteLimited()) {
    StartRewrite(db);
  }
  CheckRewrite();
  if (may_fork && ShouldAutoRewrite()) {
    RS_LOG_INFO("Starting automatic rewriting of AOF on growth\n");
    StartRewrite(db);
  }
  return true;
}
}  // namespace redis_simple::aof
