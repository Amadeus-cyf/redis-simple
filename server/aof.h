#ifndef REDIS_SIMPLE_SERVER_AOF_H_
#define REDIS_SIMPLE_SERVER_AOF_H_

#include <sys/types.h>
#include <sys/uio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
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
  // Mutations made while a rewrite runs are kept until it finishes; a rewrite
  // that would keep more is abandoned.
  size_t max_rewrite_buffer_bytes{size_t{64} * 1024 * 1024};
  size_t max_replay_command_bytes{size_t{64} * 1024 * 1024};
  // The rewrite writes the snapshot in blocks of about this size, and splits
  // large values and collections into commands no larger.
  size_t snapshot_block_bytes{size_t{1024} * 1024};
};

struct Options {
  Options() = default;
  Options(std::string file_path, FsyncPolicy fsync_policy)
      : path(std::move(file_path)), fsync(fsync_policy) {}

  std::string path{"appendonly.aof"};
  FsyncPolicy fsync{FsyncPolicy::kEverySecond};
  size_t auto_rewrite_min_bytes{size_t{64} * 1024 * 1024};
  size_t auto_rewrite_percentage{100};
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
  kRewriteBufferFull,
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
  size_t current_size{};
  size_t base_size{};
  // Encoded mutations not yet written to the file.
  size_t pending_bytes{};
};

std::string_view ErrorName(AofError error);
std::string_view RewriteStatusName(RewriteStatus status);

// The append-only file. Every method runs on the event-loop thread; with
// `everysec` a background thread only fsyncs the file once a second.
//
// Each batch of mutations is written to the file before its replies are sent,
// so a crash of this process loses no acknowledged write; with `everysec` an
// operating-system crash may lose about the last second. A rewrite forks a
// child that writes a snapshot of the database to a temporary file, while the
// parent keeps appending to the old file and also keeps the mutations made
// meanwhile; when the child exits, the parent appends those, then replaces
// the old file atomically.
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
  // a write error, and for good after a failed fsync, since the kernel may
  // have dropped the data it could not sync.
  bool AcceptingWrites() const;
  // The system error of the last failed write or sync, for MISCONF replies.
  std::string LastErrorText() const;

  // Forks a child that writes the snapshot; the server finishes the rewrite
  // through CheckRewrite once the child exits. Must run on the event-loop
  // thread while no other thread allocates memory, so the child does not
  // inherit an allocator lock held by another thread.
  RewriteResult StartRewrite(db::RedisDb* db);
  // Finishes the rewrite if its child has exited. Called by the server cron.
  void CheckRewrite();
  bool ShouldAutoRewrite() const;
  AofState State() const;

  // For tests and shutdown: flushes, then syncs the file now.
  bool SyncNow();
  // For tests: waits for the rewrite child and finishes the rewrite.
  void WaitForRewrite();

 private:
  Aof(int fd, const Options& options, size_t file_size);
  bool Replay(db::RedisDb* db);
  static bool Encode(std::string_view command,
                     const std::vector<std::string_view>& args, db::RedisDb* db,
                     std::string* output);
  bool WriteUnwritten();
  void AddWritten(size_t size);
  void FailWrite(int error_number);
  bool SyncWritten();
  void RunSyncThread();
  void FailSyncLocked(int error_number);
  void LogSyncFailure();
  void RecordError(AofError error, int error_number);
  void BufferForRewrite(std::string batch);
  [[noreturn]] void RunRewriteChild(db::RedisDb* db, int rewrite_fd) const;
  void FinishRewrite(int wait_status);
  AofError InstallRewrite(int* error_number);
  void AbandonRewrite(AofError error);
  bool SyncFile(int fd) const;
  bool SyncParentDirectory() const;

  // Guarded by `mutex_`: the file descriptor (replaced when a rewrite is
  // installed) and the sync thread's state and errors.
  mutable std::mutex mutex_;
  std::condition_variable sync_wakeup_;
  int fd_;
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
  std::string path_;
  FsyncPolicy fsync_;
  size_t auto_rewrite_min_bytes_;
  size_t auto_rewrite_percentage_;
  Limits limits_;
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
  std::thread sync_thread_;

  // The running rewrite: its child, temporary file, and the mutations made
  // since the fork, in order.
  pid_t rewrite_child_{-1};
  int rewrite_fd_{-1};
  std::string rewrite_path_;
  std::deque<std::string> rewrite_buffer_;
  size_t rewrite_buffer_bytes_{};
  RewriteStatus rewrite_status_{RewriteStatus::kNeverRun};
  std::chrono::steady_clock::time_point last_rewrite_attempt_{};
};
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_H_
