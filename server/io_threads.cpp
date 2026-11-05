#include "server/io_threads.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace redis_simple {
namespace {
// About a millisecond of polling before a worker parks, like Redis' 1M-loop
// spin in IOThreadMain.
constexpr size_t kSpinIterations = 1'000'000;
// Spins while waiting for a worker before yielding the core to it.
constexpr size_t kWaitSpinsBeforeYield = 10'000;

// Tells the CPU this is a spin-wait loop.
inline void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}
}  // namespace

std::unique_ptr<IoThreads> IoThreads::Create(size_t thread_count) {
  if (thread_count == 0) {
    return nullptr;
  }
  std::unique_ptr<IoThreads> pool(new IoThreads());
  pool->workers_.reserve(thread_count - 1);
  for (size_t index = 1; index < thread_count; ++index) {
    auto worker = std::make_unique<Worker>();
    // Workers start parked; the first large batch releases them.
    worker->park.lock();
    pool->workers_.push_back(std::move(worker));
  }
  for (size_t index = 1; index < thread_count; ++index) {
    // The project does not throw, but std::thread reports a failure to start
    // only through an exception. The destructor joins the threads that did
    // start.
    try {
      pool->workers_[index - 1]->thread = std::thread(
          [instance = pool.get(), index] { instance->WorkerLoop(index); });
    } catch (const std::system_error&) {
      return nullptr;
    }
  }
  return pool;
}

IoThreads::~IoThreads() {
  stopping_.store(true, std::memory_order_release);
  // Parked workers wake on the released mutex and see `stopping_`; awake
  // ones see it after their next spin.
  if (!active_) {
    Unpark();
  }
  for (const auto& worker : workers_) {
    if (worker->thread.joinable()) {
      worker->thread.join();
    }
  }
}

bool IoThreads::RunBatch(size_t count, JobFunction job, void* context) {
  const size_t thread_count = ThreadCount();
  if (workers_.empty() || count < kMinJobsPerThread * thread_count) {
    if (active_) {
      Park();
    }
    for (size_t index = 0; index < count; ++index) {
      job(context, index);
    }
    return false;
  }
  if (!active_) {
    Unpark();
  }
  job_ = job;
  context_ = context;
  job_count_ = count;
  for (const auto& worker : workers_) {
    worker->busy.store(true, std::memory_order_release);
  }
  for (size_t index = 0; index < count; index += thread_count) {
    job(context, index);
  }
  for (const auto& worker : workers_) {
    // A worker finishes its share quickly unless it is not scheduled, as
    // when there are more threads than cores; then yield to let it run.
    for (size_t spin = 0; worker->busy.load(std::memory_order_acquire);
         ++spin) {
      if (spin < kWaitSpinsBeforeYield) {
        CpuRelax();
      } else {
        std::this_thread::yield();
      }
    }
  }
  return true;
}

void IoThreads::WorkerLoop(size_t thread_index) {
  Worker& worker = *workers_[thread_index - 1];
  const size_t thread_count = ThreadCount();
  while (true) {
    bool busy = false;
    for (size_t spin = 0; spin < kSpinIterations; ++spin) {
      busy = worker.busy.load(std::memory_order_acquire);
      if (busy) {
        break;
      }
      CpuRelax();
    }
    if (!busy) {
      // Blocks while the calling thread holds the mutex, which it does while
      // the workers are parked.
      const std::lock_guard<std::mutex> lock(worker.park);
      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }
      continue;
    }
    for (size_t index = thread_index; index < job_count_;
         index += thread_count) {
      job_(context_, index);
    }
    worker.busy.store(false, std::memory_order_release);
  }
}

void IoThreads::Park() {
  for (const auto& worker : workers_) {
    worker->park.lock();
  }
  active_ = false;
}

void IoThreads::Unpark() {
  for (const auto& worker : workers_) {
    worker->park.unlock();
  }
  active_ = true;
}
}  // namespace redis_simple
