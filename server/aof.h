#ifndef REDIS_SIMPLE_SERVER_AOF_H_
#define REDIS_SIMPLE_SERVER_AOF_H_

#include <sys/types.h>
#include <sys/uio.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
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
  size_t max_pending_bytes{size_t{64} * 1024 * 1024};
  size_t max_pending_commands{size_t{64} * 1024};
  size_t max_replay_command_bytes{size_t{64} * 1024 * 1024};
  size_t snapshot_block_bytes{size_t{1024} * 1024};
  size_t max_snapshot_queue_bytes{size_t{4} * 1024 * 1024};
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
  kQueueFull,
  kWrite,
  kSync,
  kRename,
  kDirectorySync,
  kSnapshot,
};

struct AofState {
  bool healthy{};
  bool rewrite_in_progress{};
  RewriteStatus rewrite_status{RewriteStatus::kNeverRun};
  AofError last_error{AofError::kNone};
  size_t current_size{};
  size_t base_size{};
  size_t pending_bytes{};
};

std::string_view ErrorName(AofError error);
std::string_view RewriteStatusName(RewriteStatus status);

class Aof {
 public:
  static std::unique_ptr<Aof> Open(const Options& options, db::RedisDb* db);
  Aof(const Aof&) = delete;
  Aof& operator=(const Aof&) = delete;
  // Encodes a successful mutation into the pending batch. Nothing reaches the
  // file until Flush(), which the server calls once per batch of commands
  // read from a client, before any of their replies are sent.
  bool Append(std::string_view command,
              const std::vector<std::string_view>& args, db::RedisDb* db);
  // Hands the pending batch to the writer: with `always` it is written and
  // synced once; otherwise it is queued, waiting for space if the background
  // writer is behind. Returns false once the AOF is unhealthy.
  bool Flush();
  RewriteResult StartRewrite(db::RedisDb* db);
  bool ShouldAutoRewrite() const;
  AofState State() const;
  bool Healthy() const;
  void WaitUntilIdle();
  void WaitUntilRewriteIdle();
  ~Aof();

 private:
  class Record {
   public:
    explicit Record(std::string data) : data_(std::move(data)) {}
    explicit Record(std::shared_ptr<std::string> data)
        : data_(std::move(data)) {}
    char* Data();
    size_t Size() const;

   private:
    std::variant<std::string, std::shared_ptr<std::string>> data_;
  };

  using RecordQueue = std::deque<Record>;

  Aof(int fd, const Options& options, size_t file_size);
  bool Replay(db::RedisDb* db);
  static bool Encode(std::string_view command,
                     const std::vector<std::string_view>& args, db::RedisDb* db,
                     std::string* output);
  bool Submit(std::string batch);
  bool BuildSnapshot(db::RedisDb* db);
  bool QueueSnapshotBlock(std::string block);
  bool WriteBatch(int fd, RecordQueue& batch, std::vector<iovec>* blocks) const;
  void Run();
  void RunRewrite(int rewrite_fd, const std::string& temp_path);
  void BufferRewriteCommandLocked(const std::shared_ptr<std::string>& command);
  void FinishRewriteLocked(RewriteStatus status, AofError error);
  bool SyncFile(int fd) const;
  bool SyncParentDirectory() const;
  void FailLocked(AofError error);
  void NotifyIfIdleLocked();

  int fd_;
  std::string path_;
  FsyncPolicy fsync_;
  size_t auto_rewrite_min_bytes_;
  size_t auto_rewrite_percentage_;
  Limits limits_;
  std::shared_ptr<FileOps> file_ops_;
  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::condition_variable space_available_;
  std::condition_variable rewrite_state_changed_;
  RecordQueue pending_;
  RecordQueue snapshot_pending_;
  RecordQueue rewrite_pending_;
  // Encoded mutations not yet handed to the writer; event-loop thread only.
  std::string batch_;
  std::thread worker_;
  std::thread rewrite_worker_;
  size_t pending_bytes_{};
  size_t snapshot_pending_bytes_{};
  size_t rewrite_pending_bytes_{};
  size_t current_size_{};
  size_t base_size_{};
  bool writing_{};
  bool syncing_{};
  bool dirty_{};
  bool stopping_{};
  bool healthy_{true};
  bool rewriting_{};
  bool rewrite_failed_{};
  bool snapshot_complete_{};
  RewriteStatus rewrite_status_{RewriteStatus::kNeverRun};
  AofError last_error_{AofError::kNone};
  std::chrono::steady_clock::time_point last_sync_;
  std::chrono::steady_clock::time_point last_rewrite_attempt_{};
};
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_H_
