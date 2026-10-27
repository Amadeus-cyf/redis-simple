#ifndef REDIS_SIMPLE_UTILS_TIME_UTILS_H_
#define REDIS_SIMPLE_UTILS_TIME_UTILS_H_

#include <chrono>
#include <cstdint>

namespace redis_simple::utils {
// Wall-clock Unix time, for key expiration that must survive restarts.
inline int64_t NowInMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Monotonic time, for timers and budgets that must not jump with the clock.
inline int64_t MonotonicMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_TIME_UTILS_H_
