#include "server/idle_memory_releaser.h"

namespace redis_simple {
void IdleMemoryReleaser::Tick(const bool active) {
  if (release_ == nullptr) {
    return;
  }
  if (active) {
    pending_ = true;
    quiet_ticks_ = 0;
    return;
  }
  if (!pending_ || ++quiet_ticks_ < kQuietTicks) {
    return;
  }
  pending_ = false;
  quiet_ticks_ = 0;
  release_();
}
}  // namespace redis_simple
