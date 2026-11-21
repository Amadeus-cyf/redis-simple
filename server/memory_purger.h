#ifndef REDIS_SIMPLE_SERVER_MEMORY_PURGER_H_
#define REDIS_SIMPLE_SERVER_MEMORY_PURGER_H_

#include <atomic>
#include <cstdint>

namespace redis_simple {
// Hands the memory a thread's allocator keeps for reuse back to the
// operating system at most once a second, after any thread did work.
//
// The allocator (mimalloc) gives each thread its own heap. Free pages in it
// go back to the system only when the owning thread later allocates or
// frees a page in the same segment, or collects its heap; a page another
// thread freed into it is not even known to be free until then. Returning
// pages at every free instead costs a system call, and a page fault when
// the memory is used again, for every large value that is overwritten,
// which made a 64 KiB SET take 3.5 times the CPU. So each thread runs one of
// these from its 100 ms cron, and every tenth tick, if any thread did work
// since this thread's last purge, it calls `purge`, which collects this
// thread's heap and returns its free pages. Work on any thread counts,
// since it may have freed memory into this thread's heap: the I/O threads
// free reply buffers the main thread allocated, and the main thread frees
// the clients the I/O threads read for. Freed memory therefore goes back
// within about a second, busy or idle, as Redis' allocator returns it from
// a background thread.
class MemoryPurger {
 public:
  // Collects the calling thread's heap and returns its free pages to the
  // system. Null when the server does not allocate with mimalloc.
  using PurgeFunction = void (*)();
  // What every thread's purger shares. It must outlive them.
  struct Shared {
    PurgeFunction purge = nullptr;
    // Cron ticks, on any thread, in which that thread did work.
    std::atomic<uint64_t> work_ticks{0};
  };
  // Ticks of the owner's cron between purges: one second.
  static constexpr int kTicksPerPurge = 10;

  // Does nothing when `shared` or its function is null.
  explicit MemoryPurger(Shared* shared) : shared_(shared) {}
  // Called by the thread's cron on each tick, on that thread, with whether
  // the thread did any work since the previous tick.
  void Tick(bool did_work);

 private:
  Shared* shared_;
  // shared_->work_ticks as of this thread's last purge.
  uint64_t purged_at_{0};
  int ticks_{0};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_MEMORY_PURGER_H_
