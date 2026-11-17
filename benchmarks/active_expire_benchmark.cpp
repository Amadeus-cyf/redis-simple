#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "server/active_expire.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "utils/time.h"

// Active expiration: what a cron tick costs when nothing has expired, and how
// long back-to-back slow cycles take to reclaim a keyspace whose keys have
// all expired.
namespace redis_simple {
namespace {
constexpr int64_t kHourInMilliseconds = 3'600'000;

void Fill(db::RedisDb* db, size_t count, int64_t expire) {
  for (size_t i = 0; i < count; ++i) {
    db->SetKey("key:" + std::to_string(i),
               db::RedisObject::CreateWithString("value"), expire);
  }
}

void ActiveExpireSlowCycleNothingExpired(benchmark::State& state) {
  auto db = db::RedisDb::Create();
  Fill(db.get(), static_cast<size_t>(state.range(0)),
       utils::NowInMilliseconds() + kHourInMilliseconds);
  ActiveExpire active_expire;
  for (auto _ : state) {
    (void)_;
    active_expire.RunSlowCycle(db.get());
  }
}

void ActiveExpireReclaimExpiredKeys(benchmark::State& state) {
  const auto count = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    auto db = db::RedisDb::Create();
    Fill(db.get(), count, utils::NowInMilliseconds() - kHourInMilliseconds);
    ActiveExpire active_expire;
    state.ResumeTiming();
    while (db->KeyCount() > 0) {
      active_expire.RunSlowCycle(db.get());
    }
    state.PauseTiming();
    db.reset();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ActiveExpireSlowCycleNothingExpired)->Arg(100'000);
BENCHMARK(ActiveExpireReclaimExpiredKeys)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace
}  // namespace redis_simple
