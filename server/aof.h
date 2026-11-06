#ifndef REDIS_SIMPLE_SERVER_AOF_H_
#define REDIS_SIMPLE_SERVER_AOF_H_

#include <sys/types.h>
#include <sys/uio.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::aof {
enum class FsyncPolicy {
  kAlways,
  kEverySecond,
  kNo,
};

class FileOps {
 public:
  virtual ssize_t Write(int fd, const void* data, size_t size) = 0;
  virtual ssize_t WriteVector(int fd, const iovec* blocks, int count) = 0;
  virtual int Sync(int fd) = 0;
  virtual int Rename(const char* source, const char* destination) = 0;
  virtual ~FileOps() = default;
};

struct Limits {
  size_t max_replay_command_bytes{size_t{64} * 1024 * 1024};
  // The rewrite writes the snapshot in blocks of about this size, and splits
  // large values and collections into commands no larger.
  size_t snapshot_block_bytes{size_t{1024} * 1024};
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

// One file of a multi-part AOF, as listed in its manifest.
struct ManifestFile {
  enum class Type {
    kBase,
    kIncrement,
    // Replaced by a rewrite and deleted soon after.
    kHistory,
  };
  std::string name;
  int64_t sequence{};
  Type type{};
  // Redis' replication offsets for an incremental file. This server has no
  // replication and writes none, but keeps those of a Redis manifest.
  std::optional<int64_t> start_offset;
  std::optional<int64_t> end_offset;
};

// The manifest of a multi-part AOF, in Redis' format: one line per file, such
// as "file appendonly.aof.1.base.aof seq 1 type b". Loading replays the base
// file, then the incremental files in order.
struct Manifest {
  std::optional<ManifestFile> base;
  std::vector<ManifestFile> increments;
  std::vector<ManifestFile> history;
  // The sequence numbers of the base and the last incremental file, which the
  // next ones follow, as Redis' curr_base_file_seq and curr_incr_file_seq.
  int64_t base_sequence{};
  int64_t increment_sequence{};

  // The base first, then history files, then incremental files, as Redis
  // writes them; names that need it are quoted as by Redis' sdscatrepr.
  std::string Encode() const;
  // Parses as Redis' aofLoadManifestFromFile does, and returns nullopt where
  // it would refuse to start.
  static std::optional<Manifest> Parse(std::string_view text);
};

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
  // an automatic rewrite when the AOF has grown enough. Returns false when
  // the server must stop.
  bool Cron(db::RedisDb* db);
  // Finishes the rewrite if its child has exited.
  void CheckRewrite();
  // Whether an automatic rewrite is due: the AOF is larger than the minimum
  // size and has grown by the percentage, and repeated failures have not
  // limited rewrites (Redis' aofRewriteLimited, which this updates).
  bool ShouldAutoRewrite();
  AofState State() const;

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
  bool LoadManifest();
  bool Replay(db::RedisDb* db);
  bool ReplayFile(db::RedisDb* db, int fd, bool last);
  bool CutTail(int fd, size_t valid_bytes) const;
  std::string FilePath(std::string_view name) const;
  std::string ManifestPath() const;
  // Writes `manifest` to a temporary file, syncs it, renames it over the
  // manifest, and syncs the directory.
  AofError PersistManifest(const Manifest& manifest);
  bool CreateBaseIfMissing(db::RedisDb* db);
  bool OpenLastIncrement();
  // Creates the next incremental file, lists it in the manifest, and makes it
  // the file appends go to.
  bool OpenNextIncrement();
  void SwitchFile(int fd);
  void DeleteHistory();
  static bool Encode(std::string_view command,
                     const std::vector<std::string_view>& args, db::RedisDb* db,
                     std::string* output);
  void FailWrite(ssize_t written);
  bool FsyncInProgress() const;
  void QueueFsync(int fd, bool close_after);
  void QueueDeletion(const std::string& path);
  void RunBackgroundThread();
  void RunJob(int fd, bool close_after);
  void LogSyncFailure();
  static void RecordError(AofError error, int error_number);
  int WriteSnapshotFile(db::RedisDb* db, const std::string& path) const;
  [[noreturn]] void RunRewriteChild(db::RedisDb* db) const;
  std::string RewriteTempPath(pid_t child) const;
  void FinishRewrite(int wait_status);
  AofError InstallRewrite(pid_t child, int* error_number);
  void AbandonRewrite();
  void RemoveRewriteTempFiles(pid_t child);
  bool RewriteLimited();
  void FailDirectorySync(int error_number);
  bool SyncFile(int fd) const;
  bool SyncParentDirectory() const;

  // Shared with the background thread, guarded by `mutex_`: its queues of
  // jobs. Each job only syncs, closes, or deletes a file, and the queues are
  // cleared without freeing, so the thread never allocates or frees memory.
  mutable std::mutex mutex_;
  std::condition_variable jobs_queued_;
  std::condition_variable jobs_done_;
  struct SyncJob {
    int fd;
    // Close the file after syncing it: Redis' BIO_CLOSE_AOF.
    bool close_after;
  };
  std::vector<SyncJob> sync_jobs_;
  size_t next_sync_job_{};
  static constexpr size_t kMaxPathBytes = 4096;
  using DeletionPath = std::array<char, kMaxPathBytes>;
  std::vector<DeletionPath> deletions_;
  bool running_job_{false};
  bool stopping_{false};
  // Shared without the lock: sync jobs queued and not finished (Redis'
  // aof_pending_bio_fsync), and the result of the last background sync
  // (aof_bio_fsync_status and aof_bio_fsync_errno).
  std::atomic<size_t> pending_fsyncs_{0};
  std::atomic<bool> fsync_failed_{false};
  std::atomic<int> fsync_errno_{0};

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
  // The current incremental file.
  int fd_{-1};
  // Redis' aof_buf: encoded mutations not yet written.
  std::string batch_;
  // Redis' aof_last_write_status and aof_last_write_errno.
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
  std::thread background_thread_;

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

#endif  // REDIS_SIMPLE_SERVER_AOF_H_
