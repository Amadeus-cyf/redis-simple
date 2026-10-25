#include "expire.h"

#include <cstddef>
#include <cstdint>

#include "server/db/db.h"
#include "utils/time_utils.h"

namespace redis_simple {
void ActiveExpireCycle(db::RedisDb* const db) {
  constexpr size_t kSampleCount = 20;
  // Runs every cron tick (100 ms), so this caps expiry at 25% of the loop.
  constexpr int64_t kTimeBudgetMilliseconds = 25;
  constexpr size_t kContinueExpirePercent = 25;
  const int64_t start = utils::MonotonicMilliseconds();
  while (utils::MonotonicMilliseconds() - start < kTimeBudgetMilliseconds) {
    const auto sample =
        db->ExpireSome(kSampleCount, utils::NowInMilliseconds());
    if (sample.sampled == 0 ||
        sample.expired * 100 <= sample.sampled * kContinueExpirePercent) {
      break;
    }
  }
}
}  // namespace redis_simple
