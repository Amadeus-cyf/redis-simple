#include "server/active_expire.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "server/db/db.h"
#include "server/db/redis_object.h"

namespace redis_simple {
namespace {
constexpr int64_t kNowMilliseconds = 1'000'000;
constexpr int64_t kExpired = kNowMilliseconds - 1;
constexpr int64_t kLive = kNowMilliseconds + 60'000;

// A clock the test moves: every monotonic reading advances it by `step`.
struct FakeClock {
  int64_t monotonic_microseconds{10'000'000};
  int64_t step_microseconds{0};
  int monotonic_reads{0};
  int wall_reads{0};

  ActiveExpire::Clock Functions() {
    return {.monotonic_microseconds =
                [this] {
                  ++monotonic_reads;
                  const int64_t reading = monotonic_microseconds;
                  monotonic_microseconds += step_microseconds;
                  return reading;
                },
            .wall_milliseconds =
                [this] {
                  ++wall_reads;
                  return kNowMilliseconds;
                }};
  }
};

void AddKeys(db::RedisDb* db, const std::string& prefix, size_t count,
             int64_t expire) {
  for (size_t index = 0; index < count; ++index) {
    ASSERT_EQ(db->SetKey(prefix + std::to_string(index),
                         db::RedisObject::CreateWithString("value"), expire),
              db::DbStatus::kOk);
  }
}
}  // namespace

TEST(ActiveExpireTest, SlowCyclesDeleteExpiredKeysAndKeepLiveOnes) {
  FakeClock clock;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  AddKeys(db.get(), "expired:", 2'000, kExpired);
  AddKeys(db.get(), "live:", 200, kLive);
  AddKeys(db.get(), "persistent:", 200, 0);

  // The clock stands still, so a cycle ends only once few of the keys it
  // sampled last were expired.
  constexpr int kMaxCycles = 50;
  for (int cycle = 0; cycle < kMaxCycles && db->KeysWithExpirationCount() > 200;
       ++cycle) {
    active_expire.RunSlowCycle(db.get());
    EXPECT_FALSE(active_expire.HitTimeLimit());
  }

  EXPECT_EQ(db->KeysWithExpirationCount(), 200);
  EXPECT_EQ(db->KeyCount(), 400);
}

TEST(ActiveExpireTest, CycleReadsTheClocksRarely) {
  constexpr size_t kKeys = 3'200;
  constexpr size_t kSamplesPerRound = 20;
  FakeClock clock;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  AddKeys(db.get(), "expired:", kKeys, kExpired);

  active_expire.RunSlowCycle(db.get());

  // A round deletes its twenty samples plus the rest of the last bucket it
  // visited, taken here as at most twenty more. The budget is checked on
  // every sixteenth round, and "now" is read once.
  EXPECT_EQ(db->KeyCount(), 0);
  EXPECT_EQ(clock.wall_reads, 1);
  EXPECT_GE(clock.monotonic_reads, kKeys / (2 * kSamplesPerRound) / 16);
  EXPECT_LE(clock.monotonic_reads, kKeys / kSamplesPerRound / 4);
}

TEST(ActiveExpireTest, CycleWithoutTtlKeysDoesNotReadTheWallClock) {
  FakeClock clock;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  AddKeys(db.get(), "persistent:", 100, 0);

  active_expire.RunSlowCycle(db.get());

  EXPECT_EQ(clock.wall_reads, 0);
  EXPECT_EQ(db->KeyCount(), 100);
}

TEST(ActiveExpireTest, SlowCycleStopsAtTheFirstTimeCheckPastItsBudget) {
  FakeClock clock;
  // Every reading is 30 ms after the previous one, past the 25 ms budget.
  clock.step_microseconds = 30'000;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  AddKeys(db.get(), "expired:", 5'000, kExpired);

  active_expire.RunSlowCycle(db.get());

  // Sixteen rounds ran before the check, each deleting its twenty samples
  // plus the rest of its last bucket, taken here as fewer than forty.
  EXPECT_TRUE(active_expire.HitTimeLimit());
  EXPECT_GE(db->KeyCount(), 5'000 - 640);
  EXPECT_LT(db->KeyCount(), 5'000);
  EXPECT_DOUBLE_EQ(active_expire.StaleShare(), 0.05);
}

TEST(ActiveExpireTest, FastCycleDoesNothingWhileTheSlowCycleKeepsUp) {
  FakeClock clock;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  AddKeys(db.get(), "expired:", 100, kExpired);

  active_expire.RunFastCycle(db.get());

  EXPECT_EQ(db->KeyCount(), 100);
  EXPECT_EQ(clock.monotonic_reads, 0);
  EXPECT_EQ(clock.wall_reads, 0);
}

TEST(ActiveExpireTest, FastCycleRunsAfterTheSlowCycleRanOutOfTime) {
  FakeClock clock;
  clock.step_microseconds = 30'000;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  AddKeys(db.get(), "expired:", 5'000, kExpired);
  active_expire.RunSlowCycle(db.get());
  ASSERT_TRUE(active_expire.HitTimeLimit());
  const size_t keys_after_slow_cycle = db->KeyCount();

  // Readings 600 us apart: the 1 ms budget is exceeded at the second check,
  // after 32 rounds.
  clock.step_microseconds = 600;
  active_expire.RunFastCycle(db.get());

  EXPECT_TRUE(active_expire.HitTimeLimit());
  EXPECT_LT(db->KeyCount(), keys_after_slow_cycle);
  EXPECT_GE(db->KeyCount(), keys_after_slow_cycle - 1'280);
}

TEST(ActiveExpireTest, FastCyclesStartAtLeastTwoMillisecondsApart) {
  FakeClock clock;
  clock.step_microseconds = 30'000;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  AddKeys(db.get(), "expired:", 5'000, kExpired);
  active_expire.RunSlowCycle(db.get());
  clock.step_microseconds = 600;
  // Starts at some time T and reads the clock at T + 600 and T + 1200.
  active_expire.RunFastCycle(db.get());
  ASSERT_TRUE(active_expire.HitTimeLimit());
  const size_t keys_after_first_fast_cycle = db->KeyCount();

  // T + 1800 is too soon.
  active_expire.RunFastCycle(db.get());
  EXPECT_EQ(db->KeyCount(), keys_after_first_fast_cycle);

  // T + 2400 is not.
  active_expire.RunFastCycle(db.get());
  EXPECT_LT(db->KeyCount(), keys_after_first_fast_cycle);
}

TEST(ActiveExpireTest, FastCycleRunsWhileManySampledKeysAreStale) {
  FakeClock clock;
  ActiveExpire active_expire(clock.Functions());
  auto db = db::RedisDb::Create();
  // Each slow cycle deletes a fully expired keyspace without running out of
  // time, which still raises the stale estimate by 5% of what is left to
  // reach 1; three cycles take it past the 10% threshold.
  for (int cycle = 0; cycle < 3; ++cycle) {
    AddKeys(db.get(), "expired:", 100, kExpired);
    active_expire.RunSlowCycle(db.get());
    ASSERT_FALSE(active_expire.HitTimeLimit());
  }
  ASSERT_GT(active_expire.StaleShare(), 0.10);
  AddKeys(db.get(), "expired:", 100, kExpired);

  active_expire.RunFastCycle(db.get());

  EXPECT_LT(db->KeyCount(), 100);
}
}  // namespace redis_simple
