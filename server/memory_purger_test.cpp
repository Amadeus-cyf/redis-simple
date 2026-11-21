#include "server/memory_purger.h"

#include <gtest/gtest.h>

namespace redis_simple {
namespace {
// PurgeFunction is a plain function pointer, so the count it updates has
// static storage.
int& Purges() {
  static int count = 0;
  return count;
}

void CountPurge() { ++Purges(); }

class MemoryPurgerTest : public testing::Test {
 protected:
  void SetUp() override { Purges() = 0; }
  static void QuietTicks(MemoryPurger* purger, int count) {
    for (int i = 0; i < count; ++i) {
      purger->Tick(false);
    }
  }
};

TEST_F(MemoryPurgerTest, PurgesOnceAfterASecondWithoutWork) {
  MemoryPurger purger(&CountPurge);
  purger.Tick(true);
  QuietTicks(&purger, MemoryPurger::kQuietTicks - 1);
  EXPECT_EQ(Purges(), 0);
  purger.Tick(false);
  EXPECT_EQ(Purges(), 1);
  // Staying idle does not purge again.
  QuietTicks(&purger, 3 * MemoryPurger::kQuietTicks);
  EXPECT_EQ(Purges(), 1);
}

TEST_F(MemoryPurgerTest, WorkRestartsTheQuietPeriod) {
  MemoryPurger purger(&CountPurge);
  purger.Tick(true);
  QuietTicks(&purger, MemoryPurger::kQuietTicks - 1);
  purger.Tick(true);
  QuietTicks(&purger, MemoryPurger::kQuietTicks - 1);
  EXPECT_EQ(Purges(), 0);
  purger.Tick(false);
  EXPECT_EQ(Purges(), 1);
}

TEST_F(MemoryPurgerTest, PurgesAgainAfterMoreWork) {
  MemoryPurger purger(&CountPurge);
  purger.Tick(true);
  QuietTicks(&purger, MemoryPurger::kQuietTicks);
  purger.Tick(true);
  QuietTicks(&purger, MemoryPurger::kQuietTicks);
  EXPECT_EQ(Purges(), 2);
}

TEST_F(MemoryPurgerTest, NeverPurgesWithoutWork) {
  MemoryPurger purger(&CountPurge);
  QuietTicks(&purger, 5 * MemoryPurger::kQuietTicks);
  EXPECT_EQ(Purges(), 0);
}

TEST_F(MemoryPurgerTest, DoesNothingWithoutAFunction) {
  MemoryPurger purger(nullptr);
  purger.Tick(true);
  QuietTicks(&purger, MemoryPurger::kQuietTicks);
  EXPECT_EQ(Purges(), 0);
}
}  // namespace
}  // namespace redis_simple
