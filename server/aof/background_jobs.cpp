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
  // The sync thread stops first: it may still hand closes to the file
  // thread.
  {
    const std::scoped_lock lock(mutex_);
    stopping_sync_thread_ = true;
  }
  sync_jobs_queued_.notify_all();
  if (sync_thread_.joinable()) {
    sync_thread_.join();
  }
  for (; next_sync_job_ < sync_jobs_.size(); ++next_sync_job_) {
    const SyncJob job = sync_jobs_[next_sync_job_];
    SyncDescriptor(job.fd);
    if (job.close_after) {
      file_ops_->Close(job.fd);
    } else {
      pending_fsyncs_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
  sync_jobs_.clear();
  next_sync_job_ = 0;
  {
    const std::scoped_lock lock(mutex_);
    promised_closes_ = 0;
    stopping_file_thread_ = true;
  }
  file_jobs_queued_.notify_all();
  if (file_thread_.joinable()) {
    file_thread_.join();
  }
  for (const int fd : closes_) {
    file_ops_->Close(fd);
  }
  for (const DeletionPath& path : deletions_) {
    file_ops_->Unlink(path.data());
  }
  closes_.clear();
  deletions_.clear();
}

bool BackgroundJobs::Start() {
  // The project does not throw, but std::thread reports a failure to start
  // only by throwing std::system_error; catch it at this boundary so the
  // caller sees an ordinary error.
  try {
    sync_thread_ = std::thread([this] { RunSyncThread(); });
    file_thread_ = std::thread([this] { RunFileThread(); });
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
    if (close_after) {
      // Allocates here, on the caller's thread, so that the sync thread's
      // push does not.
      ++promised_closes_;
      closes_.reserve(closes_.size() + promised_closes_);
    }
    sync_jobs_.push_back({fd, close_after});
  }
  sync_jobs_queued_.notify_all();
}

void BackgroundJobs::QueueDeletion(const std::string& path) {
  DeletionPath copy{};
  if (path.size() >= copy.size()) {
    file_ops_->Unlink(path.c_str());
    return;
  }
  std::ranges::copy(path, copy.begin());
  {
    const std::scoped_lock lock(mutex_);
    deletions_.push_back(copy);
  }
  file_jobs_queued_.notify_all();
}

void BackgroundJobs::WaitUntilIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  jobs_done_.wait(lock, [this] {
    return next_sync_job_ == sync_jobs_.size() && !FileJobsQueued() &&
           !running_sync_job_ && !running_file_job_;
  });
}

void BackgroundJobs::SyncDescriptor(int fd) {
  // A file already closed is not a failed sync.
  if (!SyncFile(file_ops_, fd) && errno != EBADF && errno != EINVAL) {
    fsync_errno_.store(errno, std::memory_order_relaxed);
    fsync_failed_.store(true, std::memory_order_release);
  } else {
    fsync_failed_.store(false, std::memory_order_release);
  }
}

void BackgroundJobs::RunSyncThread() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    sync_jobs_queued_.wait(lock, [this] {
      return stopping_sync_thread_ || next_sync_job_ < sync_jobs_.size();
    });
    running_sync_job_ = true;
    while (next_sync_job_ < sync_jobs_.size()) {
      const SyncJob job = sync_jobs_[next_sync_job_++];
      lock.unlock();
      SyncDescriptor(job.fd);
      if (!job.close_after) {
        pending_fsyncs_.fetch_sub(1, std::memory_order_acq_rel);
      }
      lock.lock();
      if (job.close_after) {
        // QueueFsync reserved the slot, so this does not allocate.
        closes_.push_back(job.fd);
        --promised_closes_;
        file_jobs_queued_.notify_all();
      }
    }
    // Keeps the capacity: clearing trivially destructible jobs frees nothing.
    sync_jobs_.clear();
    next_sync_job_ = 0;
    running_sync_job_ = false;
    jobs_done_.notify_all();
    if (stopping_sync_thread_) {
      return;
    }
  }
}

void BackgroundJobs::RunFileThread() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    file_jobs_queued_.wait(
        lock, [this] { return stopping_file_thread_ || FileJobsQueued(); });
    running_file_job_ = true;
    while (FileJobsQueued()) {
      if (!closes_.empty()) {
        const int fd = closes_.back();
        closes_.pop_back();
        lock.unlock();
        file_ops_->Close(fd);
      } else {
        const DeletionPath path = deletions_.back();
        deletions_.pop_back();
        lock.unlock();
        file_ops_->Unlink(path.data());
      }
      lock.lock();
    }
    running_file_job_ = false;
    jobs_done_.notify_all();
    if (stopping_file_thread_) {
      return;
    }
  }
}
}  // namespace redis_simple::aof
