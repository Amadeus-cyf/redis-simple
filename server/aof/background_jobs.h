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

// The background threads for file work, like Redis' bio threads, so the
// event loop never waits for the disk. As in Redis' bio.c, each class of job
// has its own thread and queue, so a slow job of one class does not hold up
// the other:
//
// - the sync thread syncs files, in the order queued (Redis' BIO_AOF_FSYNC);
// - the file thread closes files and deletes them (Redis' BIO_CLOSE_FILE).
//   Closing the last descriptor of a deleted file, or deleting a large file,
//   frees its blocks, which can take seconds.
//
// A file to sync and then close (Redis' BIO_CLOSE_AOF) is synced on the sync
// thread, which hands the close to the file thread.
//
// A rewrite or a background save forks while these threads may be running,
// and the child must not inherit a held allocator lock. So the threads never
// allocate, free, or log: deletion paths live in fixed-size buffers, the
// queues keep their capacity when cleared, and the caller's thread reserves
// the room for a close before the sync thread hands it over.
class BackgroundJobs {
 public:
  // `file_ops` must outlive this object.
  explicit BackgroundJobs(FileOps* file_ops) : file_ops_(file_ops) {}
  BackgroundJobs(const BackgroundJobs&) = delete;
  BackgroundJobs& operator=(const BackgroundJobs&) = delete;
  ~BackgroundJobs() { Stop(); }

  // Starts the threads. False if one could not start; jobs then run in
  // Stop().
  bool Start();
  // Syncs `fd`, then closes it if `close_after` (Redis' BIO_CLOSE_AOF).
  // Syncs that keep the file open count as pending until they finish.
  void QueueFsync(int fd, bool close_after);
  // Deletes the file at `path`; deletes it at once if the path is too long
  // for the thread's fixed-size buffer.
  void QueueDeletion(const std::string& path);
  // Waits until every job queued so far has finished.
  void WaitUntilIdle();
  // Lets the threads finish the queued jobs and stop, then runs any jobs
  // they did not, such as all of them when they never started.
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

  void RunSyncThread();
  void RunFileThread();
  void SyncDescriptor(int fd);
  bool FileJobsQueued() const {
    return !closes_.empty() || !deletions_.empty();
  }

  FileOps* const file_ops_;
  // Guards the queues and flags below.
  std::mutex mutex_;
  std::condition_variable sync_jobs_queued_;
  std::condition_variable file_jobs_queued_;
  std::condition_variable jobs_done_;
  std::vector<SyncJob> sync_jobs_;
  size_t next_sync_job_{};
  // Descriptors the file thread closes. QueueFsync reserves a slot for each
  // close the sync thread will hand over; `promised_closes_` counts those
  // not handed over yet.
  std::vector<int> closes_;
  size_t promised_closes_{};
  std::vector<DeletionPath> deletions_;
  bool running_sync_job_{false};
  bool running_file_job_{false};
  bool stopping_sync_thread_{false};
  bool stopping_file_thread_{false};
  // Read without the lock.
  std::atomic<size_t> pending_fsyncs_{0};
  std::atomic<bool> fsync_failed_{false};
  std::atomic<int> fsync_errno_{0};
  std::thread sync_thread_;
  std::thread file_thread_;
};
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_BACKGROUND_JOBS_H_
