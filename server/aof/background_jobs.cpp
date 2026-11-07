#include "server/aof/background_jobs.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "server/aof/file_ops.h"

namespace redis_simple::aof {
void BackgroundJobs::Stop() {
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  jobs_queued_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  for (; next_sync_job_ < sync_jobs_.size(); ++next_sync_job_) {
    RunSyncJob(sync_jobs_[next_sync_job_]);
  }
  for (const DeletionPath& path : deletions_) {
    unlink(path.data());
  }
  sync_jobs_.clear();
  next_sync_job_ = 0;
  deletions_.clear();
}

bool BackgroundJobs::Start() {
  // The project does not throw, but std::thread reports a failure to start
  // only by throwing std::system_error; catch it at this boundary so the
  // caller sees an ordinary error.
  try {
    thread_ = std::thread([this] { Run(); });
  } catch (const std::system_error&) {
    return false;
  }
  return true;
}

void BackgroundJobs::QueueFsync(int fd, bool close_after) {
  if (!close_after) {
    pending_fsyncs_.fetch_add(1, std::memory_order_acq_rel);
  }
  {
    const std::scoped_lock lock(mutex_);
    sync_jobs_.push_back({fd, close_after});
  }
  jobs_queued_.notify_all();
}

void BackgroundJobs::QueueDeletion(const std::string& path) {
  DeletionPath copy{};
  if (path.size() >= copy.size()) {
    unlink(path.c_str());
    return;
  }
  std::ranges::copy(path, copy.begin());
  {
    const std::scoped_lock lock(mutex_);
    deletions_.push_back(copy);
  }
  jobs_queued_.notify_all();
}

void BackgroundJobs::WaitUntilIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  jobs_done_.wait(lock, [this] {
    return next_sync_job_ == sync_jobs_.size() && deletions_.empty() &&
           !running_job_;
  });
}

void BackgroundJobs::RunSyncJob(const SyncJob& job) {
  // A file already closed is not a failed sync.
  if (!SyncFile(file_ops_, job.fd) && errno != EBADF && errno != EINVAL) {
    fsync_errno_.store(errno, std::memory_order_relaxed);
    fsync_failed_.store(true, std::memory_order_release);
  } else {
    fsync_failed_.store(false, std::memory_order_release);
  }
  if (job.close_after) {
    close(job.fd);
  } else {
    pending_fsyncs_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

void BackgroundJobs::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    jobs_queued_.wait(lock, [this] {
      return stopping_ || next_sync_job_ < sync_jobs_.size() ||
             !deletions_.empty();
    });
    running_job_ = true;
    while (next_sync_job_ < sync_jobs_.size()) {
      const SyncJob job = sync_jobs_[next_sync_job_++];
      lock.unlock();
      RunSyncJob(job);
      lock.lock();
    }
    // Keeps the capacity: clearing trivially destructible jobs frees nothing.
    sync_jobs_.clear();
    next_sync_job_ = 0;
    while (!deletions_.empty()) {
      const DeletionPath path = deletions_.back();
      deletions_.pop_back();
      lock.unlock();
      unlink(path.data());
      lock.lock();
    }
    running_job_ = false;
    jobs_done_.notify_all();
    if (stopping_) {
      return;
    }
  }
}
}  // namespace redis_simple::aof
