#include "server/db/async_reclaimer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

namespace redis_simple::db {
TEST(AsyncReclaimerTest, ReclaimsObjectsOnWorkerThread) {
  AsyncReclaimer reclaimer;
  EXPECT_FALSE(reclaimer.Reclaim(nullptr));

  constexpr size_t kObjectCount = 32;
  for (size_t index = 0; index < kObjectCount; ++index) {
    ASSERT_TRUE(reclaimer.Reclaim(
        RedisObject::CreateWithString(std::string(4096, 'x'))));
  }

  reclaimer.WaitUntilIdle();
  EXPECT_EQ(reclaimer.PendingCount(), 0);
}

TEST(AsyncReclaimerTest, PausedWorkerFreesNothingUntilResumed) {
  AsyncReclaimer reclaimer;
  reclaimer.Pause();
  ASSERT_TRUE(reclaimer.Reclaim(RedisObject::CreateWithString("value")));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Still queued: the worker does not touch the heap while paused.
  EXPECT_EQ(reclaimer.PendingCount(), 1);

  reclaimer.Resume();
  reclaimer.WaitUntilIdle();
  EXPECT_EQ(reclaimer.PendingCount(), 0);
}
}  // namespace redis_simple::db
