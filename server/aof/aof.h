#ifndef REDIS_SIMPLE_SERVER_AOF_AOF_H_
#define REDIS_SIMPLE_SERVER_AOF_AOF_H_

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "server/aof/background_jobs.h"
#include "server/aof/file_ops.h"
#include "server/aof/manifest.h"
#include "server/aof/snapshot.h"

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::aof {
enum class FsyncPolicy {
  kAlways,
  kEverySecond,
  kNo,
};

struct Options {
  Options() = default;
  Options(std::string aof_directory, std::string aof_file_name,
          FsyncPolicy fsync_policy)
      : directory(std::move(aof_directory)),
        file_name(std::move(aof_file_name)),
        fsync(fsync_policy) {}

  // Redis' appenddirname: the directory holding the AOF's files and manifest.
  std::string directory{"appendonlydir"};
  // Redis' appendfilename: the prefix of the AOF's file names, without a
  // directory. An existing single-file AOF of this name in the working
  // directory is moved into `directory` as the base file.
  std::string file_name{"appendonly.aof"};
  FsyncPolicy fsync{FsyncPolicy::kEverySecond};
  size_t auto_rewrite_min_bytes{size_t{64} * 1024 * 1024};
  size_t auto_rewrite_percentage{100};
  // Redis' aof-load-truncated: a last file that ends in a torn record is
  // cut back to its last complete command instead of refusing to load.
  bool load_truncated{true};
  // Redis' aof-load-corrupt-tail-max-size: a format error in the last file
  // is cut off when fewer than this many bytes follow the last complete
  // command. 0, the default, refuses to load instead.
  size_t load_corrupt_tail_max_bytes{0};
  // Redis' aof-timestamp-enabled: a "#TS:<unix time>" annotation precedes
  // the first command appended in each second.
  bool timestamp_enabled{false};
  // Redis' aof-rewrite-incremental-fsync: the rewrite child syncs every
  // 4 MiB it writes, instead of only at the end.
  bool rewrite_incremental_fsync{true};
  // Redis' no-appendfsync-on-rewrite: no fsync while a rewrite runs.
  bool no_fsync_on_rewrite{false};
  Limits limits;
  std::shared_ptr<FileOps> file_ops;
};

enum class RewriteResult {
  kStarted,
  kInProgress,
  kError,
};

enum class AofError {
  kNone,
  kWrite,
  kSync,
  kRename,
  kDirectorySync,
  kSnapshot,
};

// The AOF fields of Redis' INFO persistence section.
struct AofState {
  // aof_last_write_status: false after a write or fsync error, while write
  // commands are rejected.
  bool accepting_writes{};
  bool rewrite_in_progress{};
  // aof_rewrite_scheduled: a BGREWRITEAOF waiting for the cron.
  bool rewrite_scheduled{};
  // aof_last_bgrewrite_status.
  bool last_rewrite_ok{true};
  // aof_last_rewrite_time_sec and aof_current_rewrite_time_sec; -1 if none.
  int64_t last_rewrite_seconds{-1};
  int64_t current_rewrite_seconds{-1};
  // aof_rewrites: rewrites started.
  size_t rewrites{};
  size_t consecutive_rewrite_failures{};
  // aof_current_size: the base and incremental files. aof_base_size: the
  // size after the last rewrite, or the base file's at startup, which
  // automatic rewrites measure growth from.
  size_t current_size{};
  size_t base_size{};
  // aof_buffer_length: encoded mutations not yet written.
  size_t buffer_length{};
  // aof_pending_bio_fsync and aof_delayed_fsync.
  size_t pending_fsyncs{};
  size_t delayed_fsyncs{};
};

std::string_view ErrorName(AofError error);

// A multi-part append-only file, as in Redis 8.10: a directory with one base
// file (the last rewrite's snapshot), incremental files with the mutations
// since, and a manifest listing them. Every method runs on the event-loop
// thread. A background thread, like Redis' bio thread, syncs files, closes
// replaced ones, and deletes retired ones.
//
// Mutations are encoded into a buffer that Flush() writes before their
// replies are sent. With `always` each write is synced; with `everysec` the
// background thread syncs once a second, and a write waits up to two seconds
// for a sync still running, as in Redis. A rewrite first switches appends to
// a new incremental file, then forks a child that writes a snapshot of the
// database as the next base file. When the child finishes, a new manifest
// lists that base and the new incremental file, and the files it replaced
// are deleted.
class Aof {
 public:
  static std::unique_ptr<Aof> Open(const Options& options, db::RedisDb* db);
  Aof(const Aof&) = delete;
  Aof& operator=(const Aof&) = delete;
  Aof(Aof&&) = delete;
  Aof& operator=(Aof&&) = delete;
  // Kills a running rewrite, then writes and syncs everything pending.
  ~Aof();

  // Encodes a successful mutation into the buffer. Nothing reaches the file
  // until Flush(), which the server calls once per batch of commands read
  // from a client, before any of their replies are sent.
  bool Append(std::string_view command,
              const std::vector<std::string_view>& args, db::RedisDb* db);
  // Brackets the mutations of one EXEC. When it appended more than one
  // record, EndTransaction wraps them in MULTI/EXEC records so replay applies
  // the transaction completely or, if the file ends inside it, not at all.
  void BeginTransaction();
  void EndTransaction();
  // Redis' flushAppendOnlyFile: writes the buffer, and syncs as the policy
  // asks. With `everysec` and no `force`, the write waits, for up to two
  // seconds, while a background sync runs. A failed write keeps the buffer,
  // cutting a partial write back off the file, and rejects write commands
  // until a later write succeeds. Returns false only with `always`, after a
  // failed write or sync: the server must then stop.
  bool Flush(bool force = false);
  // Whether write commands may run: no write or sync error is outstanding.
  // A failed sync clears with the next successful one.
  bool AcceptingWrites() const;
  // The system error behind the last write or sync failure, for MISCONF.
  std::string LastErrorText() const;

  // Redis' rewriteAppendOnlyFileBackground: forks a child that writes the
  // snapshot; Cron() or CheckRewrite() finishes the rewrite once it exits.
  // No other thread may allocate during the fork, or the child could inherit
  // a held allocator lock: the AOF background thread never does, and
  // `before_fork` must stop any other thread that might, such as the I/O
  // threads, until `after_fork`.
  RewriteResult StartRewrite(db::RedisDb* db);
  // BGREWRITEAOF inside EXEC: the cron starts the rewrite afterwards, and
  // the failure backoff is reset, as in Redis.
  void ScheduleRewrite();
  bool RewriteInProgress() const { return rewrite_child_ > 0; }
  void SetForkHooks(std::function<void()> before_fork,
                    std::function<void()> after_fork) {
    before_fork_ = std::move(before_fork);
    after_fork_ = std::move(after_fork);
  }
  // The AOF part of Redis' serverCron: flushes a postponed or failed write,
  // starts a scheduled rewrite, finishes one whose child exited, and starts
  // an automatic rewrite when the AOF has grown enough. It starts none while
  // `may_fork` is false, when another child (a background save) runs, as
  // Redis runs one child at a time. Returns false when the server must stop.
  bool Cron(db::RedisDb* db, bool may_fork = true);
  // Finishes the rewrite if its child has exited.
  void CheckRewrite();
  // Whether an automatic rewrite is due: the AOF is larger than the minimum
  // size and has grown by the percentage, and repeated failures have not
  // limited rewrites (Redis' aofRewriteLimited, which this updates).
  bool ShouldAutoRewrite();
  AofState State() const;
  // Changes the loaded files made, which Redis counts toward the changes
  // since the last RDB save.
  int64_t ReplayedChanges() const { return replayed_changes_; }

  // For tests and shutdown: writes everything pending, then syncs the file.
  bool SyncNow();
  // For tests: waits for the rewrite child and finishes the rewrite.
  void WaitForRewrite();
  // For tests: waits until the background thread has finished every job
  // queued so far.
  void WaitForBackgroundJobs();
  // The manifest as last persisted.
  const Manifest& CurrentManifest() const { return manifest_; }

 private:
  Aof(const Options& options, std::string directory);

  // Startup and loading (aof_load.cpp).
  bool LoadManifest();
  bool Replay(db::RedisDb* db);
  bool ReplayFile(db::RedisDb* db, int fd, bool last);
  bool HandleTruncatedFile(int fd, bool last, size_t valid_bytes) const;
  bool HandleCorruptTail(int fd, bool last, size_t corrupt_from) const;
  bool CutTail(int fd, size_t valid_bytes) const;
  bool CreateBaseIfMissing(db::RedisDb* db);
  bool OpenLastIncrement();

  // Rewriting (aof_rewrite.cpp).
  [[noreturn]] void RunRewriteChild(db::RedisDb* db) const;
  std::string RewriteTempPath(pid_t child) const;
  void FinishRewrite(int wait_status);
  AofError InstallRewrite(pid_t child, int* error_number);
  void AbandonRewrite();
  void RemoveRewriteTempFiles(pid_t child);
  bool RewriteLimited();

  // Files and the manifest (aof.cpp).
  std::string FilePath(std::string_view name) const;
  std::string ManifestPath() const;
  // Writes `manifest` to a temporary file, syncs it, renames it over the
  // manifest, and syncs the directory.
  AofError PersistManifest(const Manifest& manifest);
  // Creates the next incremental file, lists it in the manifest, and makes it
  // the file appends go to.
  bool OpenNextIncrement();
  void SwitchFile(int fd);
  void DeleteHistory();
  void FailDirectorySync(int error_number);
  void FailWrite(ssize_t written);
  void LogSyncFailure();
  static void RecordError(AofError error, int error_number);
  SnapshotOptions SnapshotSettings() const;

  // Event-loop thread only.
  std::string directory_;
  std::string file_name_;
  Manifest manifest_;
  FsyncPolicy fsync_;
  size_t auto_rewrite_min_bytes_;
  size_t auto_rewrite_percentage_;
  Limits limits_;
  bool load_truncated_;
  size_t load_corrupt_tail_max_bytes_;
  bool timestamp_enabled_;
  bool rewrite_incremental_fsync_;
  bool no_fsync_on_rewrite_;
  std::shared_ptr<FileOps> file_ops_;
  // Syncs, closes, and deletes files on its own thread.
  BackgroundJobs jobs_;
  // The current incremental file.
  int fd_{-1};
  // Redis' aof_buf: encoded mutations not yet written.
  std::string batch_;
  // Whether the last write failed, and why (Redis' aof_last_write_status and
  // aof_last_write_errno).
  bool write_failed_{false};
  int write_errno_{0};
  std::chrono::steady_clock::time_point last_write_error_log_{};
  bool fsync_failure_logged_{false};
  // Where the open EXEC's records start in `batch_`, and how many it has.
  size_t transaction_start_{};
  size_t transaction_records_{};
  bool in_transaction_{};
  // Whether a SELECT was written since startup or the last rewrite started,
  // as Redis' aof_selected_db.
  bool database_selected_{false};
  // Redis' aof_cur_timestamp, for timestamp annotations.
  int64_t current_timestamp_{};
  int64_t replayed_changes_{};
  // Redis' aof_current_size, aof_rewrite_base_size, aof_last_incr_size, and
  // aof_last_incr_fsync_offset.
  size_t current_size_{};
  size_t base_size_{};
  size_t increment_size_{};
  size_t increment_fsync_offset_{};
  // Redis' aof_last_fsync, aof_flush_postponed_start, and aof_delayed_fsync.
  std::chrono::steady_clock::time_point last_fsync_{};
  std::optional<std::chrono::steady_clock::time_point> flush_postponed_start_;
  size_t delayed_fsyncs_{};

  // The running rewrite's child, and Redis' rewrite statistics.
  pid_t rewrite_child_{-1};
  std::chrono::steady_clock::time_point rewrite_start_{};
  int64_t last_rewrite_seconds_{-1};
  bool rewrite_scheduled_{false};
  bool last_rewrite_ok_{true};
  size_t rewrites_{};
  size_t consecutive_rewrite_failures_{};
  // aofRewriteLimited's state: the current delay, and when it ends.
  int64_t next_delay_minutes_{};
  std::optional<std::chrono::steady_clock::time_point> next_rewrite_time_;
  // Set when a manifest was renamed into place but its directory sync
  // failed: a crash could bring back the previous manifest. Writes are
  // rejected and history files kept until a later manifest persists safely.
  bool manifest_unsafe_{false};
  int manifest_errno_{0};
  std::function<void()> before_fork_;
  std::function<void()> after_fork_;
};
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_AOF_H_
