#ifndef REDIS_SIMPLE_SERVER_IDLE_MEMORY_RELEASER_H_
#define REDIS_SIMPLE_SERVER_IDLE_MEMORY_RELEASER_H_

#include <cstdint>

namespace redis_simple {
// Hands the memory a thread's allocator keeps for reuse back to the
// operating system once that thread has been idle for a second.
//
// The allocator (mimalloc) returns freed pages to the system only after a
// short delay, and only while the thread that owns them keeps allocating.
// Returning them on every free instead costs a system call, and a page
// fault when the memory is used again, for every large value that is
// overwritten, which made a 64 KiB SET take 3.5 times the CPU. With
// the delay, a busy server reuses freed pages, but one that deletes a large
// value and then idles would keep the memory indefinitely, so each thread
// that frees memory runs one of these from its cron: after a second without
// activity it calls `release` once, which collects that thread's heap. Like
// Redis' allocator, whose background thread returns freed memory after a
// delay, the server gives memory back within seconds of going idle.
class IdleMemoryReleaser {
 public:
  // Collects the calling thread's heap and returns its free pages to the
  // system. Null when the server does not allocate with mimalloc.
  using ReleaseFunction = void (*)();
  // How many quiet ticks of the owner's cron make the thread idle.
  static constexpr int kQuietTicks = 10;

  explicit IdleMemoryReleaser(ReleaseFunction release) : release_(release) {}
  // Called by the thread's cron on each tick, with whether the thread did
  // any work since the previous tick. Calls `release` once after the first
  // kQuietTicks quiet ticks that follow work.
  void Tick(bool active);

 private:
  ReleaseFunction release_;
  // Whether there was work since the last release, and how many quiet ticks
  // have passed since it.
  bool pending_{false};
  int quiet_ticks_{0};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_IDLE_MEMORY_RELEASER_H_
