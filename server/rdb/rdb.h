#ifndef REDIS_SIMPLE_SERVER_RDB_RDB_H_
#define REDIS_SIMPLE_SERVER_RDB_RDB_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace redis_simple::aof {
class FileOps;
}

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::rdb {
// One of Redis' save points: a background save starts once `changes` changes
// have been made and more than `seconds` seconds have passed since the last
// save.
struct SavePoint {
  int64_t seconds;
  int64_t changes;

  bool operator==(const SavePoint&) const = default;
};

struct Options {
  // Redis' dbfilename: the RDB file's name in the working directory.
  std::string file_name{"dump.rdb"};
  // Redis' rdbcompression and rdbchecksum.
  bool compression{true};
  bool checksum{true};
  // Redis' save, as Redis 8 sets it when started without a config file.
  // Empty turns automatic saves, and the save at shutdown, off.
  std::vector<SavePoint> save_points{{3600, 1}, {300, 100}, {60, 10000}};
  // Redis' stop-writes-on-bgsave-error: refuse writes while the last
  // background save failed, when there are save points.
  bool stop_writes_on_error{true};
  // Tests inject failures here; null uses the real system calls.
  std::shared_ptr<aof::FileOps> file_ops;
};

// The RDB fields of INFO persistence.
struct RdbState {
  int64_t changes_since_last_save{};
  bool background_save_in_progress{};
  int64_t last_save_time{};
  bool last_background_save_ok{true};
  // -1 before the first background save.
  int64_t last_background_save_seconds{-1};
  // -1 when no background save is running.
  int64_t current_background_save_seconds{-1};
  int64_t saves{};
  int64_t consecutive_failures{};
  size_t last_load_keys_expired{};
  size_t last_load_keys_loaded{};
};

enum class BackgroundSaveResult : uint8_t {
  kStarted,
  kInProgress,
  kError,
};

// The server's RDB snapshots (Redis' rdb.c): loading the file at startup,
// SAVE, BGSAVE's forked child, the save points, and the save at shutdown.
// Only one child runs at a time across the server, so callers check the
// AOF's rewrite before starting a background save.
class Rdb {
 public:
  static std::unique_ptr<Rdb> Create(Options options);
  Rdb(const Rdb&) = delete;
  Rdb& operator=(const Rdb&) = delete;
  // Stops a running background save, as shutdown does.
  ~Rdb();

  // Loads the file into `db`, as Redis does at startup when the AOF is off.
  // A missing file leaves `db` empty. Returns false after logging why the
  // file could not be loaded, which should stop the server.
  bool Load(db::RedisDb* db);

  // SAVE, as Redis' rdbSave does: writes "temp-<pid>.rdb", renames it over
  // the file, and syncs the directory. Returns false after logging a
  // failure, leaving the previous file in place.
  bool Save(db::RedisDb* db);

  // BGSAVE, as Redis' rdbSaveBackground does: forks a child that saves as
  // Save does, without logging. Cron() finishes it once the child exits.
  // No thread may allocate during the fork; see SetForkHooks.
  BackgroundSaveResult StartBackgroundSave(db::RedisDb* db);
  // BGSAVE SCHEDULE, or BGSAVE while another child runs: Cron() starts the
  // save once no child does.
  void ScheduleBackgroundSave() { background_save_scheduled_ = true; }
  bool BackgroundSaveInProgress() const { return child_ > 0; }
  void SetForkHooks(std::function<void()> before_fork,
                    std::function<void()> after_fork) {
    before_fork_ = std::move(before_fork);
    after_fork_ = std::move(after_fork);
  }

  // The RDB part of Redis' serverCron: finishes a background save whose
  // child exited, and otherwise starts one when a save point is reached or
  // one is scheduled. `may_fork` is false while another child (an AOF
  // rewrite) runs.
  void Cron(db::RedisDb* db, bool may_fork);

  // The RDB part of Redis' prepareForShutdown: stops a background save, then
  // saves when there are save points. Returns false if that save failed,
  // in which case the server keeps running, as Redis does.
  bool SaveBeforeShutdown(db::RedisDb* db);

  // Redis' server.dirty: changes made since the last successful save.
  void AddChanges(int64_t changes) { changes_ += changes; }
  // The counter itself, which clients add to directly, so counting a
  // command's changes costs one addition.
  int64_t* ChangeCounter() { return &changes_; }

  // Redis' writeCommandsDeniedByDiskError for RDB: writes are refused while
  // the last background save failed, if there are save points and
  // stop-writes-on-bgsave-error is on.
  bool DeniesWrites() const { return denies_writes_; }

  RdbState State() const;

  // LASTSAVE: when the last successful save finished, in Unix seconds; the
  // server's start time before any.
  int64_t LastSaveTime() const { return last_save_time_; }

  const std::string& FileName() const { return options_.file_name; }

 private:
  // How saving the file ended. A background save's child exits with it.
  enum class SaveStatus : uint8_t {
    kSaved = 0,
    kWriteFailed = 1,
    kSyncFailed = 2,
    kRenameFailed = 3,
    kDirectorySyncFailed = 4,
  };

  explicit Rdb(Options options);

  // Writes `temporary`, renames it over the file, and syncs the directory,
  // without logging, so the child can run it.
  SaveStatus WriteAndRename(db::RedisDb* db,
                            const std::string& temporary) const;
  // rdbSave: Save without counting a SAVE command.
  bool SaveNow(db::RedisDb* db);
  [[noreturn]] void RunChild(db::RedisDb* db) const;
  void CheckBackgroundSave();
  void FinishBackgroundSave(int wait_status);
  // Stops the child without recording a failure (Redis' killRDBChild).
  void AbandonBackgroundSave();
  void SetLastBackgroundSaveOk(bool ok);
  // The first save point reached at `now`, if any.
  const SavePoint* ReachedSavePoint(int64_t now) const;

  Options options_;
  std::function<void()> before_fork_;
  std::function<void()> after_fork_;
  pid_t child_{-1};
  bool background_save_scheduled_{};
  int64_t changes_{};
  // Changes made before the running background save started, which it
  // covers.
  int64_t changes_before_save_{};
  int64_t last_save_time_;
  // When the last background save was tried, to wait between retries after
  // a failure; 0 before any.
  int64_t last_try_time_{};
  bool last_background_save_ok_{true};
  bool denies_writes_{};
  int64_t background_save_start_{-1};
  int64_t last_background_save_seconds_{-1};
  int64_t saves_{};
  int64_t consecutive_failures_{};
  size_t last_load_keys_expired_{};
  size_t last_load_keys_loaded_{};
};
}  // namespace redis_simple::rdb

#endif  // REDIS_SIMPLE_SERVER_RDB_RDB_H_
