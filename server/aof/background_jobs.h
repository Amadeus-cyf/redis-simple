#ifndef REDIS_SIMPLE_SERVER_AOF_BACKGROUND_JOBS_H_
#define REDIS_SIMPLE_SERVER_AOF_BACKGROUND_JOBS_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace redis_simple::aof {
class FileOps;

// The AOF's background thread, like Redis' bio thread: it syncs files,
// closes files a rewrite replaced, and deletes files a rewrite retired, so
// the event loop never waits for the disk.
//
// The rewrite forks while this thread may be running, and the child must not
// inherit a held allocator lock. So the thread never allocates, frees, or
// logs: deletion paths live in fixed-size buffers, and its queues keep their
// capacity when cleared.
class BackgroundJobs {
 public:
  // `file_ops` must outlive this object.
  explicit BackgroundJobs(FileOps* file_ops) : file_ops_(file_ops) {}
  BackgroundJobs(const BackgroundJobs&) = delete;
  BackgroundJobs& operator=(const BackgroundJobs&) = delete;
  ~BackgroundJobs() { Stop(); }

  // Starts the thread. False if it could not start; jobs then run in the
  // destructor.
  bool Start();
  // Syncs `fd`, then closes it if `close_after` (Redis' BIO_CLOSE_AOF).
  // Syncs that keep the file open count as pending until they finish.
  void QueueFsync(int fd, bool close_after);
  // Deletes the file at `path`; deletes it at once if the path is too long
  // for the thread's fixed-size buffer.
  void QueueDeletion(const std::string& path);
  // Waits until every job queued so far has finished.
  void WaitUntilIdle();
  // Lets the thread finish the queued jobs and stop, then runs any jobs it
  // did not, such as all of them when it never started.
  void Stop();

  // Syncs queued or running that keep their file open (Redis'
  // aof_pending_bio_fsync).
  size_t PendingFsyncs() const {
    return pending_fsyncs_.load(std::memory_order_acquire);
  }
  // Whether the last background sync failed, and why (Redis'
  // aof_bio_fsync_status and aof_bio_fsync_errno). A later successful sync
  // clears it.
  bool FsyncFailed() const {
    return fsync_failed_.load(std::memory_order_acquire);
  }
  int FsyncErrno() const {
    return fsync_errno_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kMaxPathBytes = 4096;
  using DeletionPath = std::array<char, kMaxPathBytes>;
  struct SyncJob {
    int fd;
    bool close_after;
  };

  void Run();
  void RunSyncJob(const SyncJob& job);

  FileOps* const file_ops_;
  // Guards the queues and flags below.
  std::mutex mutex_;
  std::condition_variable jobs_queued_;
  std::condition_variable jobs_done_;
  std::vector<SyncJob> sync_jobs_;
  size_t next_sync_job_{};
  std::vector<DeletionPath> deletions_;
  bool running_job_{false};
  bool stopping_{false};
  // Read without the lock.
  std::atomic<size_t> pending_fsyncs_{0};
  std::atomic<bool> fsync_failed_{false};
  std::atomic<int> fsync_errno_{0};
  std::thread thread_;
};
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_BACKGROUND_JOBS_H_
