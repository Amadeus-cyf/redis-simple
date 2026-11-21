#include "server/memory_purger.h"

namespace redis_simple {
void MemoryPurger::Tick(const bool did_work) {
  if (purge_ == nullptr) {
    return;
  }
  if (did_work) {
    pending_ = true;
    quiet_ticks_ = 0;
    return;
  }
  if (!pending_ || ++quiet_ticks_ < kQuietTicks) {
    return;
  }
  pending_ = false;
  quiet_ticks_ = 0;
  purge_();
}
}  // namespace redis_simple
