#include "server/active_expire.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "server/db/db.h"
#include "utils/time.h"

namespace redis_simple {
namespace {
constexpr size_t kSamplesPerRound = 20;
// A round is repeated while more than this share of its sample was expired.
constexpr size_t kAcceptableStalePercent = 10;
// The cron runs every 100 ms, so the slow cycle takes about 25% of the loop.
constexpr int64_t kSlowCycleBudgetMicroseconds = 25'000;
constexpr int64_t kFastCycleBudgetMicroseconds = 1'000;
// The budget is checked once per this many rounds, as in Redis, which keeps
// the clock read out of most rounds. A cycle can overrun by up to that many.
constexpr size_t kRoundsPerTimeCheck = 16;
// Weight of the latest cycle in the running stale estimate.
constexpr double kStaleShareWeight = 0.05;

bool TooManyExpired(const db::ExpireSampleResult& sample) {
  return sample.expired * 100 / sample.sampled > kAcceptableStalePercent;
}
}  // namespace

ActiveExpire::ActiveExpire()
    : ActiveExpire(Clock{.monotonic_microseconds = utils::MonotonicMicroseconds,
                         .wall_milliseconds = utils::NowInMilliseconds}) {}

ActiveExpire::ActiveExpire(Clock clock) : clock_(std::move(clock)) {}

void ActiveExpire::RunSlowCycle(db::RedisDb* const db) {
  RunCycle(db, clock_.monotonic_microseconds(), kSlowCycleBudgetMicroseconds);
}

void ActiveExpire::RunFastCycle(db::RedisDb* const db) {
  // Checked first: this runs on every event-loop iteration, and when the
  // slow cycle keeps up it should not even read the clock.
  const bool slow_cycle_keeps_up =
      !hit_time_limit_ && stale_share_ * 100 < kAcceptableStalePercent;
  if (slow_cycle_keeps_up) {
    return;
  }
  const int64_t start = clock_.monotonic_microseconds();
  if (start <
      last_fast_cycle_microseconds_ + (2 * kFastCycleBudgetMicroseconds)) {
    return;
  }
  last_fast_cycle_microseconds_ = start;
  RunCycle(db, start, kFastCycleBudgetMicroseconds);
}

void ActiveExpire::RunCycle(db::RedisDb* const db,
                            const int64_t start_microseconds,
                            const int64_t budget_microseconds) {
  hit_time_limit_ = false;
  size_t total_sampled = 0;
  size_t total_expired = 0;
  if (db->KeysWithExpirationCount() > 0) {
    // One reading serves the whole cycle: a cycle lasts about 25 ms, and a
    // key that expires during it is caught by the next one.
    const int64_t now = clock_.wall_milliseconds();
    for (size_t round = 1;; ++round) {
      const db::ExpireSampleResult sample =
          db->ExpireSome(kSamplesPerRound, now);
      total_sampled += sample.sampled;
      total_expired += sample.expired;
      if (sample.sampled == 0 || !TooManyExpired(sample)) {
        break;
      }
      if (round % kRoundsPerTimeCheck == 0 &&
          clock_.monotonic_microseconds() - start_microseconds >
              budget_microseconds) {
        hit_time_limit_ = true;
        break;
      }
    }
  }
  const double cycle_share = total_sampled == 0
                                 ? 0.0
                                 : static_cast<double>(total_expired) /
                                       static_cast<double>(total_sampled);
  stale_share_ = (cycle_share * kStaleShareWeight) +
                 (stale_share_ * (1.0 - kStaleShareWeight));
}
}  // namespace redis_simple
