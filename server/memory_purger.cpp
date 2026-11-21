#include "server/memory_purger.h"

#include <atomic>
#include <cstdint>

namespace redis_simple {
void MemoryPurger::Tick(const bool did_work) {
  if (shared_ == nullptr || shared_->purge == nullptr) {
    return;
  }
  if (did_work) {
    // With the acquire below, the frees this thread made before this tick
    // happen before the purge of any thread that sees the increment.
    shared_->work_ticks.fetch_add(1, std::memory_order_release);
  }
  if (++ticks_ < kTicksPerPurge) {
    return;
  }
  ticks_ = 0;
  const uint64_t work_ticks =
      shared_->work_ticks.load(std::memory_order_acquire);
  if (work_ticks == purged_at_) {
    return;
  }
  purged_at_ = work_ticks;
  shared_->purge();
}
}  // namespace redis_simple
