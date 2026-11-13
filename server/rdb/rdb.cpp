#include "server/rdb/rdb.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "logging/logger.h"
#include "server/aof/file_ops.h"
#include "server/db/db.h"
#include "server/rdb/rdb_loader.h"
#include "server/rdb/rdb_writer.h"
#include "utils/time_utils.h"

namespace redis_simple::rdb {
namespace {
// After a failed background save, save points and scheduled saves wait this
// long before trying again (Redis' CONFIG_BGSAVE_RETRY_DELAY).
constexpr int64_t kRetryDelaySeconds = 5;

int64_t UnixSeconds() { return utils::NowInMilliseconds() / 1000; }

std::string TemporaryFileName(pid_t pid) {
  return "temp-" + std::to_string(pid) + ".rdb";
}

pid_t WaitForChild(pid_t child, int* const wait_status, int options) {
  while (true) {
    const pid_t result = waitpid(child, wait_status, options);
    if (result >= 0 || errno != EINTR) {
      return result;
    }
  }
}
}  // namespace

Rdb::Rdb(Options options)
    : options_(std::move(options)), last_save_time_(UnixSeconds()) {
  if (options_.file_ops == nullptr) {
    options_.file_ops = aof::DefaultFileOps();
  }
}

Rdb::~Rdb() {
  if (child_ > 0) {
    AbandonBackgroundSave();
  }
}

std::unique_ptr<Rdb> Rdb::Create(Options options) {
  return std::unique_ptr<Rdb>(new Rdb(std::move(options)));
}

bool Rdb::Load(db::RedisDb* const db) {
  const int64_t start = utils::NowInMilliseconds();
  const int fd = aof::OpenFile(options_.file_name, O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      return true;
    }
    RS_LOG_ERROR("Fatal error opening the RDB file %s: %s\n",
                 options_.file_name.c_str(), std::strerror(errno));
    return false;
  }
  LoadStats stats;
  const bool loaded =
      LoadRdb(fd, db, {.now_ms = utils::NowInMilliseconds()}, &stats);
  close(fd);
  if (!loaded) {
    RS_LOG_ERROR("Fatal error loading the DB, check server logs. Exiting.\n");
    return false;
  }
  last_load_keys_loaded_ = stats.keys_loaded;
  last_load_keys_expired_ = stats.keys_expired;
  RS_LOG_INFO(
      "Done loading RDB, keys loaded: %zu, keys expired: %zu, empty keys "
      "skipped: %zu.\n",
      stats.keys_loaded, stats.keys_expired, stats.empty_keys_skipped);
  RS_LOG_INFO("DB loaded from disk: %.3f seconds\n",
              static_cast<double>(utils::NowInMilliseconds() - start) / 1000);
  return true;
}

Rdb::SaveStatus Rdb::WriteAndRename(db::RedisDb* const db,
                                    const std::string& temporary) const {
  const WriteStatus status = WriteRdbFile(db, temporary,
                                          {.compression = options_.compression,
                                           .checksum = options_.checksum,
                                           .creation_time = UnixSeconds()},
                                          options_.file_ops.get());
  if (status != WriteStatus::kWritten) {
    return status == WriteStatus::kSyncFailed ? SaveStatus::kSyncFailed
                                              : SaveStatus::kWriteFailed;
  }
  if (options_.file_ops->Rename(temporary.c_str(), options_.file_name.c_str()) <
      0) {
    const int error_number = errno;
    unlink(temporary.c_str());
    errno = error_number;
    return SaveStatus::kRenameFailed;
  }
  if (!aof::SyncDirectory(options_.file_ops.get(), ".")) {
    return SaveStatus::kDirectorySyncFailed;
  }
  return SaveStatus::kSaved;
}

bool Rdb::Save(db::RedisDb* const db) {
  ++saves_;
  return SaveNow(db);
}

bool Rdb::SaveNow(db::RedisDb* const db) {
  const std::string temporary = TemporaryFileName(getpid());
  switch (WriteAndRename(db, temporary)) {
    case SaveStatus::kSaved:
      break;
    case SaveStatus::kWriteFailed:
    case SaveStatus::kSyncFailed:
      RS_LOG_WARN("Write error while saving DB to the disk: %s\n",
                  std::strerror(errno));
      return false;
    case SaveStatus::kRenameFailed:
      RS_LOG_WARN(
          "Error moving temp DB file %s on the final destination %s: %s\n",
          temporary.c_str(), options_.file_name.c_str(), std::strerror(errno));
      return false;
    case SaveStatus::kDirectorySyncFailed:
      RS_LOG_WARN("Failed to fsync directory while saving DB: %s\n",
                  std::strerror(errno));
      return false;
  }
  RS_LOG_INFO("DB saved on disk\n");
  changes_ = 0;
  last_save_time_ = UnixSeconds();
  SetLastBackgroundSaveOk(true);
  return true;
}

BackgroundSaveResult Rdb::StartBackgroundSave(db::RedisDb* const db) {
  if (child_ > 0) {
    return BackgroundSaveResult::kInProgress;
  }
  ++saves_;
  changes_before_save_ = changes_;
  last_try_time_ = UnixSeconds();
  if (before_fork_) {
    before_fork_();
  }
  const pid_t child = fork();
  if (child == 0) {
    RunChild(db);
  }
  const int fork_errno = errno;
  if (after_fork_) {
    after_fork_();
  }
  if (child < 0) {
    SetLastBackgroundSaveOk(false);
    RS_LOG_WARN("Can't save in background: fork: %s\n",
                std::strerror(fork_errno));
    return BackgroundSaveResult::kError;
  }
  RS_LOG_INFO("Background saving started by pid %d\n", static_cast<int>(child));
  child_ = child;
  background_save_start_ = UnixSeconds();
  return BackgroundSaveResult::kStarted;
}

void Rdb::RunChild(db::RedisDb* const db) const {
  // Redis' child names the temporary file after its own pid.
  _exit(static_cast<int>(WriteAndRename(db, TemporaryFileName(getpid()))));
}

void Rdb::Cron(db::RedisDb* const db, bool may_fork) {
  if (child_ > 0) {
    CheckBackgroundSave();
    return;
  }
  if (!may_fork) {
    return;
  }
  const int64_t now = UnixSeconds();
  // A failed save is retried once the delay has passed.
  const bool may_retry =
      last_background_save_ok_ || now - last_try_time_ > kRetryDelaySeconds;
  if (const SavePoint* const point =
          may_retry ? ReachedSavePoint(now) : nullptr) {
    RS_LOG_INFO("%" PRId64 " changes in %" PRId64 " seconds. Saving...\n",
                point->changes, point->seconds);
    StartBackgroundSave(db);
    return;
  }
  if (background_save_scheduled_ && may_retry &&
      StartBackgroundSave(db) == BackgroundSaveResult::kStarted) {
    background_save_scheduled_ = false;
  }
}

const SavePoint* Rdb::ReachedSavePoint(int64_t now) const {
  for (const SavePoint& point : options_.save_points) {
    if (changes_ >= point.changes && now - last_save_time_ > point.seconds) {
      return &point;
    }
  }
  return nullptr;
}

void Rdb::CheckBackgroundSave() {
  int wait_status = 0;
  const pid_t result = WaitForChild(child_, &wait_status, WNOHANG);
  if (result != 0) {
    FinishBackgroundSave(result < 0 ? -1 : wait_status);
  }
}

void Rdb::FinishBackgroundSave(int wait_status) {
  // Redis' backgroundSaveDoneHandlerDisk.
  const int64_t end = UnixSeconds();
  const pid_t child = child_;
  child_ = -1;
  if (wait_status != -1 && WIFEXITED(wait_status) &&
      WEXITSTATUS(wait_status) == static_cast<int>(SaveStatus::kSaved)) {
    RS_LOG_INFO("Background saving terminated with success\n");
    // Changes made while the child saved are not in the file.
    changes_ -= changes_before_save_;
    last_save_time_ = end;
    SetLastBackgroundSaveOk(true);
    consecutive_failures_ = 0;
  } else if (wait_status != -1 && WIFSIGNALED(wait_status)) {
    RS_LOG_WARN("Background saving terminated by signal %d\n",
                WTERMSIG(wait_status));
    unlink(TemporaryFileName(child).c_str());
    // SIGUSR1 is how a save is stopped on purpose, not a failure.
    if (WTERMSIG(wait_status) != SIGUSR1) {
      SetLastBackgroundSaveOk(false);
      ++consecutive_failures_;
    }
  } else {
    RS_LOG_WARN("Background saving error\n");
    SetLastBackgroundSaveOk(false);
    ++consecutive_failures_;
  }
  last_background_save_seconds_ = end - background_save_start_;
  background_save_start_ = -1;
}

void Rdb::AbandonBackgroundSave() {
  if (kill(child_, SIGUSR1) == 0) {
    int wait_status = 0;
    WaitForChild(child_, &wait_status, 0);
  }
  unlink(TemporaryFileName(child_).c_str());
  child_ = -1;
  background_save_start_ = -1;
}

bool Rdb::SaveBeforeShutdown(db::RedisDb* const db) {
  // Redis kills the child first, so it cannot replace the file this save
  // writes.
  if (child_ > 0) {
    RS_LOG_WARN("There is a child saving an .rdb. Killing it!\n");
    AbandonBackgroundSave();
  }
  if (options_.save_points.empty()) {
    return true;
  }
  RS_LOG_INFO("Saving the final RDB snapshot before exiting.\n");
  if (!SaveNow(db)) {
    RS_LOG_WARN("Error trying to save the DB, can't exit.\n");
    return false;
  }
  return true;
}

void Rdb::SetLastBackgroundSaveOk(bool ok) {
  last_background_save_ok_ = ok;
  denies_writes_ =
      !ok && options_.stop_writes_on_error && !options_.save_points.empty();
}

RdbState Rdb::State() const {
  return {
      .changes_since_last_save = changes_,
      .background_save_in_progress = child_ > 0,
      .last_save_time = last_save_time_,
      .last_background_save_ok = last_background_save_ok_,
      .last_background_save_seconds = last_background_save_seconds_,
      .current_background_save_seconds =
          child_ > 0 ? UnixSeconds() - background_save_start_ : -1,
      .saves = saves_,
      .consecutive_failures = consecutive_failures_,
      .last_load_keys_expired = last_load_keys_expired_,
      .last_load_keys_loaded = last_load_keys_loaded_,
  };
}
}  // namespace redis_simple::rdb
