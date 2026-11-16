#ifndef REDIS_SIMPLE_SERVER_ACTIVE_EXPIRE_H_
#define REDIS_SIMPLE_SERVER_ACTIVE_EXPIRE_H_

#include <cstdint>
#include <functional>

namespace redis_simple {
namespace db {
class RedisDb;
}  // namespace db

// Deletes expired keys that no client asks for, as Redis' activeExpireCycle
// does: each round samples keys with a TTL, deletes the expired ones, and
// repeats while too many of the sample were expired. The slow cycle runs
// from the server cron. The fast cycle runs before the event loop sleeps,
// and only while the slow cycle is not keeping up.
class ActiveExpire {
 public:
  struct Clock {
    // For time budgets; must not go backwards.
    std::function<int64_t()> monotonic_microseconds;
    // Unix time, which key expiration times are compared with.
    std::function<int64_t()> wall_milliseconds;
  };

  // Reads the system clocks.
  ActiveExpire();
  explicit ActiveExpire(Clock clock);

  // Spends at most a quarter of a 100 ms cron tick.
  void RunSlowCycle(db::RedisDb* db);
  // Spends about a millisecond. Does nothing unless the previous cycle ran
  // out of time or more than a tenth of recently sampled keys were expired,
  // and never starts within two milliseconds of the previous fast cycle.
  void RunFastCycle(db::RedisDb* db);

  bool HitTimeLimit() const { return hit_time_limit_; }
  // Running estimate of the share of keys with a TTL that are expired but
  // not yet deleted, from 0 to 1 (Redis' stat_expired_stale_perc).
  double StaleShare() const { return stale_share_; }

 private:
  void RunCycle(db::RedisDb* db, int64_t start_microseconds,
                int64_t budget_microseconds);

  Clock clock_;
  bool hit_time_limit_{false};
  double stale_share_{0.0};
  int64_t last_fast_cycle_microseconds_{0};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_ACTIVE_EXPIRE_H_
