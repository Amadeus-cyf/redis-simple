#ifndef REDIS_SIMPLE_SERVER_IO_THREADS_H_
#define REDIS_SIMPLE_SERVER_IO_THREADS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace redis_simple {
// Threaded I/O counters, reported by INFO stats with Redis' field names.
struct IoStats {
  // Whether the I/O threads are awake, which they are only under load.
  bool active{false};
  // Client reads and writes done by batches shared across the threads.
  uint64_t threaded_reads{0};
  uint64_t threaded_writes{0};
};

// Runs a batch of independent jobs, such as one socket read or write per
// client, on the calling thread plus worker threads, like Redis' threaded
// I/O. Run returns only after every job has finished, so the caller can use
// the results without further synchronization.
//
// Workers spin briefly while waiting for a batch, then park on a mutex that
// the calling thread holds while batches are too small to share. Run must
// always be called from the same thread, which owns the parking mutexes.
class IoThreads {
 public:
  // `thread_count` includes the calling thread, so 1 creates no workers.
  // Returns nullptr if a worker thread fails to start.
  static std::unique_ptr<IoThreads> Create(size_t thread_count);
  IoThreads(const IoThreads&) = delete;
  IoThreads& operator=(const IoThreads&) = delete;
  IoThreads(IoThreads&&) = delete;
  IoThreads& operator=(IoThreads&&) = delete;
  // Stops and joins the workers.
  ~IoThreads();

  size_t ThreadCount() const { return workers_.size() + 1; }
  // Whether the workers are awake, which happens once a batch is large
  // enough to share.
  bool Active() const { return active_; }

  // Calls job(index) for every index in [0, count) and returns when all have
  // finished. Thread t of ThreadCount() runs the indices congruent to t, and
  // the calling thread is thread 0. A batch with fewer than
  // kMinJobsPerThread jobs per thread runs on the calling thread alone and
  // parks the workers. Returns whether the batch was shared.
  template <typename Job>
  bool Run(size_t count, Job& job) {
    return RunBatch(
        count,
        [](void* context, size_t index) {
          (*static_cast<Job*>(context))(index);
        },
        &job);
  }

  static constexpr size_t kMinJobsPerThread = 2;

 private:
  using JobFunction = void (*)(void* context, size_t index);
  struct Worker {
    std::thread thread;
    // Held by the calling thread while the workers are parked.
    std::mutex park;
    // Set by the calling thread when a batch is published, and cleared by
    // the worker once its share is done.
    std::atomic<bool> busy{false};
  };

  IoThreads() = default;
  bool RunBatch(size_t count, JobFunction job, void* context);
  void WorkerLoop(size_t thread_index);
  void Park();
  void Unpark();

  std::vector<std::unique_ptr<Worker>> workers_;
  // The current batch. Written before `busy` is set (release) and read by
  // workers after they observe it (acquire).
  JobFunction job_{nullptr};
  void* context_{nullptr};
  size_t job_count_{0};
  std::atomic<bool> stopping_{false};
  // Calling thread only: whether the parking mutexes are released.
  bool active_{false};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_IO_THREADS_H_
