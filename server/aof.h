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
  // Redis 8.4's aof-load-corrupt-tail-max-size: a format error in the last
  // file's final bytes is cut off, like a torn record, when it spans at most
  // this many bytes. 0, the default, refuses to load instead.
  size_t load_corrupt_tail_max_bytes{0};
  Limits limits;
  std::shared_ptr<FileOps> file_ops;
};

enum class RewriteResult {
  kStarted,
  kInProgress,
  kError,
};

enum class RewriteStatus {
  kNeverRun,
  kSucceeded,
  kFailed,
};

enum class AofError {
  kNone,
  kWrite,
  kSync,
  kRename,
  kDirectorySync,
  kSnapshot,
};

struct AofState {
  // False after a write or sync error; write commands are then rejected.
  bool accepting_writes{};
  bool rewrite_in_progress{};
  RewriteStatus rewrite_status{RewriteStatus::kNeverRun};
  AofError last_error{AofError::kNone};
  // Bytes in the base and incremental files, and in the base file alone.
  size_t current_size{};
  size_t base_size{};
  // Encoded mutations not yet written to the file.
  size_t pending_bytes{};
  // Failed rewrites since the last successful one, as Redis'
  // aof_rewrites_consecutive_failures.
  size_t consecutive_rewrite_failures{};
};

std::string_view ErrorName(AofError error);
std::string_view RewriteStatusName(RewriteStatus status);

// One file of a multi-part AOF, as listed in its manifest.
struct ManifestFile {
  enum class Type {
    kBase,
    kIncrement,
    // Replaced by a rewrite and deleted soon after.
    kHistory,
  };
  std::string name;
  uint64_t sequence{};
  Type type{};
};

// The manifest of a multi-part AOF, in Redis 7's format: one line per file,
// such as "file appendonly.aof.1.base.aof seq 1 type b". Loading replays the
// base file, then the incremental files in order.
struct Manifest {
  std::optional<ManifestFile> base;
  std::vector<ManifestFile> increments;
  std::vector<ManifestFile> history;
  // The highest sequence numbers used so far.
  uint64_t base_sequence{};
  uint64_t increment_sequence{};

  std::string Encode() const;
  // Returns nullopt for a malformed manifest.
  static std::optional<Manifest> Parse(std::string_view text);
};

// A multi-part append-only file, like Redis 7's: a directory with one base
// file (the last rewrite's snapshot), incremental files with the mutations
// since, and a manifest listing them. Every method runs on the event-loop
// thread; with `everysec` a background thread only fsyncs, once a second.
//
// Each batch of mutations is written to the current incremental file before
// its replies are sent, so a crash of this process loses no acknowledged
// write; with `everysec` an operating-system crash may lose about the last
// second. A rewrite first switches appends to a new incremental file, then
// forks a child that writes a snapshot of the database as a new base file.
// When the child finishes, a new manifest lists that base and the new
// incremental file, and the files it replaced are deleted.
class Aof {
 public:
  static std::unique_ptr<Aof> Open(const Options& options, db::RedisDb* db);
  Aof(const Aof&) = delete;
  Aof& operator=(const Aof&) = delete;
  Aof(Aof&&) = delete;
  Aof& operator=(Aof&&) = delete;
  // Stops a running rewrite, flushes and syncs the file.
  ~Aof();

  // Encodes a successful mutation into the pending batch. Nothing reaches the
  // file until Flush(), which the server calls once per batch of commands
  // read from a client, before any of their replies are sent.
  bool Append(std::string_view command,
              const std::vector<std::string_view>& args, db::RedisDb* db);
  // Brackets the mutations of one EXEC. When it appended more than one
  // record, EndTransaction wraps them in MULTI/EXEC records so replay applies
  // the transaction completely or, if the file ends inside it, not at all.
  void BeginTransaction();
  void EndTransaction();
  // Writes the pending batch, and with `always` syncs it. Returns false only
  // with `always` when that fails: the batch is then not durable and its
  // replies must not be sent. With the other policies a failed write keeps
  // the bytes, retries them on later calls, and makes AcceptingWrites()
  // false until a write succeeds.
  bool Flush();
  // Whether write commands may run. False while written bytes are stuck after
  // a write error, and after a failed fsync until a rewrite retires the file
  // it failed on, since the kernel may have dropped the data it could not
  // sync.
  bool AcceptingWrites() const;
  // The system error of the last failed write or sync, for MISCONF replies.
  std::string LastErrorText() const;

  // Forks a child that writes the snapshot; the server finishes the rewrite
  // through CheckRewrite once the child exits. No other thread may allocate
  // during the fork, or the child could inherit a held allocator lock: the
  // AOF background thread never does, and `before_fork` must stop any other
  // thread that might, such as the I/O threads, until `after_fork`.
  RewriteResult StartRewrite(db::RedisDb* db);
  void SetForkHooks(std::function<void()> before_fork,
                    std::function<void()> after_fork) {
    before_fork_ = std::move(before_fork);
    after_fork_ = std::move(after_fork);
  }
  // Finishes the rewrite if its child has exited. Called by the server cron.
  void CheckRewrite();
  bool ShouldAutoRewrite() const;
  AofState State() const;

  // For tests and shutdown: flushes, then syncs the file now.
  bool SyncNow();
  // For tests: waits for the rewrite child and finishes the rewrite.
  void WaitForRewrite();
  // For tests: waits until the background thread has closed and deleted
  // every file queued so far.
  void WaitForBackgroundJobs();
  // The manifest as last persisted.
  const Manifest& CurrentManifest() const { return manifest_; }

 private:
  Aof(const Options& options, std::string directory);
  bool LoadManifest();
  bool Replay(db::RedisDb* db);
  bool ReplayFile(db::RedisDb* db, int fd, bool last);
  bool RepairCorruptTail(int fd, size_t valid_bytes) const;
  std::string FilePath(std::string_view name) const;
  std::string ManifestPath() const;
  // Writes `manifest` to a temporary file, syncs it, renames it over the
  // manifest, and syncs the directory.
  AofError PersistManifest(const Manifest& manifest);
  // Creates the next incremental file, lists it in the manifest, and makes it
  // the file appends go to.
  bool OpenLastIncrement();
  bool OpenNextIncrement();
  void SwitchFile(int fd, uint64_t sequence);
  void DeleteHistory();
  static bool Encode(std::string_view command,
                     const std::vector<std::string_view>& args, db::RedisDb* db,
                     std::string* output);
  bool WriteUnwritten();
  void AddWritten(size_t size);
  void FailWrite(int error_number);
  bool SyncWritten();
  void RunBackgroundThread();
  void QueueDeletion(const std::string& path);
  void SweepUnlistedFiles();
  void FailSyncLocked(int error_number, uint64_t sequence);
  void LogSyncFailure();
  void RecordError(AofError error, int error_number);
  [[noreturn]] void RunRewriteChild(db::RedisDb* db, int rewrite_fd) const;
  void FinishRewrite(int wait_status);
  AofError InstallRewrite(int* error_number);
  void AbandonRewrite(AofError error);
  void FailRewrite();
  void FailDirectorySync(int error_number);
  bool SyncFile(int fd) const;
  bool SyncParentDirectory() const;

  // Guarded by `mutex_`: the current incremental file (replaced when a
  // rewrite starts), files it replaced that still need a sync before they
  // close, and the sync thread's state and errors.
  mutable std::mutex mutex_;
  std::condition_variable sync_wakeup_;
  int fd_{-1};
  uint64_t fd_sequence_{};
  struct RetiredFile {
    int fd;
    uint64_t sequence;
    // written_bytes_ when the file was replaced.
    uint64_t written_mark;
    // `everysec` syncs the file's last writes before closing it.
    bool needs_sync;
  };
  std::vector<RetiredFile> retired_files_;
  // Files to delete. Fixed-size buffers, so the background thread never
  // allocates or frees memory; the event-loop thread only appends.
  static constexpr size_t kMaxPathBytes = 4096;
  using DeletionPath = std::array<char, kMaxPathBytes>;
  std::vector<DeletionPath> deletions_;
  bool running_job_{false};
  std::condition_variable jobs_done_;
  // The incremental file a sync failed on.
  uint64_t sync_failed_sequence_{};
  bool stopping_{false};
  // Also read without the lock by AcceptingWrites().
  std::atomic<bool> sync_failed_{false};
  AofError last_error_{AofError::kNone};
  int last_errno_{0};
  // Bytes written and bytes known synced, counted across file replacements;
  // the sync thread syncs while they differ.
  std::atomic<uint64_t> written_bytes_{0};
  uint64_t synced_bytes_{0};

  // Event-loop thread only.
  std::string directory_;
  std::string file_name_;
  Manifest manifest_;
  FsyncPolicy fsync_;
  size_t auto_rewrite_min_bytes_;
  size_t auto_rewrite_percentage_;
  Limits limits_;
  size_t load_corrupt_tail_max_bytes_;
  std::shared_ptr<FileOps> file_ops_;
  // Encoded mutations of the current batch.
  std::string batch_;
  // Flushed bytes a failed write left unwritten, retried by the next Flush.
  std::string unwritten_;
  bool write_failed_{false};
  bool sync_failure_logged_{false};
  // Where the open EXEC's records start in `batch_`, and how many it has.
  size_t transaction_start_{};
  size_t transaction_records_{};
  bool in_transaction_{};
  size_t current_size_{};
  size_t base_size_{};
  std::thread background_thread_;

  // The running rewrite: its child, the temporary base file, and the first
  // incremental file after the fork, which the new base precedes.
  pid_t rewrite_child_{-1};
  int rewrite_fd_{-1};
  std::string rewrite_path_;
  uint64_t rewrite_increment_sequence_{};
  RewriteStatus rewrite_status_{RewriteStatus::kNeverRun};
  size_t consecutive_rewrite_failures_{};
  // Set when a manifest rename may not be durable; history files then stay
  // until a later manifest persists safely.
  bool history_unsafe_to_delete_{false};
  std::chrono::steady_clock::time_point last_rewrite_attempt_{};
  std::function<void()> before_fork_;
  std::function<void()> after_fork_;
};
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_H_
