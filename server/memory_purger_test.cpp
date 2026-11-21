#include "server/memory_purger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace redis_simple {
namespace {
// PurgeFunction is a plain function pointer, so the count it updates has
// static storage. It is atomic for the test that ticks from several threads.
std::atomic<int>& Purges() {
  static std::atomic<int> count{0};
  return count;
}

void CountPurge() { ++Purges(); }

class MemoryPurgerTest : public testing::Test {
 protected:
  void SetUp() override { Purges() = 0; }
  static void Ticks(MemoryPurger* purger, int count, bool did_work) {
    for (int i = 0; i < count; ++i) {
      purger->Tick(did_work);
    }
  }
};

TEST_F(MemoryPurgerTest, NeverPurgesWithoutWork) {
  MemoryPurger::Shared shared{.purge = &CountPurge};
  MemoryPurger purger(&shared);
  Ticks(&purger, 5 * MemoryPurger::kTicksPerPurge, false);
  EXPECT_EQ(Purges(), 0);
}

TEST_F(MemoryPurgerTest, PurgesOnceAfterWorkThenStops) {
  MemoryPurger::Shared shared{.purge = &CountPurge};
  MemoryPurger purger(&shared);
  purger.Tick(true);
  Ticks(&purger, MemoryPurger::kTicksPerPurge - 2, false);
  EXPECT_EQ(Purges(), 0);
  // The tenth tick purges, within a second of the work.
  purger.Tick(false);
  EXPECT_EQ(Purges(), 1);
  // Staying idle does not purge again.
  Ticks(&purger, 3 * MemoryPurger::kTicksPerPurge, false);
  EXPECT_EQ(Purges(), 1);
}

TEST_F(MemoryPurgerTest, PurgesOnceASecondWhileBusy) {
  MemoryPurger::Shared shared{.purge = &CountPurge};
  MemoryPurger purger(&shared);
  // Work on every tick never postpones the purge.
  for (int second = 1; second <= 3; ++second) {
    Ticks(&purger, MemoryPurger::kTicksPerPurge - 1, true);
    EXPECT_EQ(Purges(), second - 1);
    purger.Tick(true);
    EXPECT_EQ(Purges(), second);
  }
}

TEST_F(MemoryPurgerTest, WorkAfterAPurgeWaitsForTheNextSecond) {
  MemoryPurger::Shared shared{.purge = &CountPurge};
  MemoryPurger purger(&shared);
  Ticks(&purger, MemoryPurger::kTicksPerPurge, true);
  EXPECT_EQ(Purges(), 1);
  purger.Tick(true);
  Ticks(&purger, MemoryPurger::kTicksPerPurge - 2, false);
  EXPECT_EQ(Purges(), 1);
  purger.Tick(false);
  EXPECT_EQ(Purges(), 2);
}

TEST_F(MemoryPurgerTest, AnotherThreadsWorkMakesThisThreadPurge) {
  MemoryPurger::Shared shared{.purge = &CountPurge};
  // The main thread did nothing itself, but an I/O thread freed memory,
  // possibly into the main thread's heap.
  MemoryPurger main_thread(&shared);
  MemoryPurger io_thread(&shared);
  io_thread.Tick(true);
  Ticks(&io_thread, MemoryPurger::kTicksPerPurge - 1, false);
  EXPECT_EQ(Purges(), 1);
  Ticks(&main_thread, MemoryPurger::kTicksPerPurge, false);
  EXPECT_EQ(Purges(), 2);
  // Neither purges again until some thread works.
  Ticks(&io_thread, MemoryPurger::kTicksPerPurge, false);
  Ticks(&main_thread, MemoryPurger::kTicksPerPurge, false);
  EXPECT_EQ(Purges(), 2);
}

TEST_F(MemoryPurgerTest, ThreadsTickTheSharedCountConcurrently) {
  MemoryPurger::Shared shared{.purge = &CountPurge};
  constexpr int kThreads = 4;
  constexpr int kSeconds = 50;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&shared] {
      MemoryPurger purger(&shared);
      Ticks(&purger, kSeconds * MemoryPurger::kTicksPerPurge, true);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  // Each thread worked in every second, so each purged once a second.
  EXPECT_EQ(Purges(), kThreads * kSeconds);
  EXPECT_EQ(shared.work_ticks.load(),
            uint64_t{kThreads} * kSeconds * MemoryPurger::kTicksPerPurge);
}

TEST_F(MemoryPurgerTest, DoesNothingWithoutAFunction) {
  MemoryPurger::Shared without_function;
  MemoryPurger purger(&without_function);
  Ticks(&purger, 2 * MemoryPurger::kTicksPerPurge, true);
  MemoryPurger without_shared(nullptr);
  Ticks(&without_shared, 2 * MemoryPurger::kTicksPerPurge, true);
  EXPECT_EQ(Purges(), 0);
}
}  // namespace
}  // namespace redis_simple
