#include "server/idle_memory_releaser.h"

#include <gtest/gtest.h>

namespace redis_simple {
namespace {
// ReleaseFunction is a plain function pointer, so the count it updates has
// static storage.
int& Releases() {
  static int count = 0;
  return count;
}

void CountRelease() { ++Releases(); }

class IdleMemoryReleaserTest : public testing::Test {
 protected:
  void SetUp() override { Releases() = 0; }
  static void QuietTicks(IdleMemoryReleaser* releaser, int count) {
    for (int i = 0; i < count; ++i) {
      releaser->Tick(false);
    }
  }
};

TEST_F(IdleMemoryReleaserTest, ReleasesOnceAfterASecondWithoutWork) {
  IdleMemoryReleaser releaser(&CountRelease);
  releaser.Tick(true);
  QuietTicks(&releaser, IdleMemoryReleaser::kQuietTicks - 1);
  EXPECT_EQ(Releases(), 0);
  releaser.Tick(false);
  EXPECT_EQ(Releases(), 1);
  // Staying idle does not release again.
  QuietTicks(&releaser, 3 * IdleMemoryReleaser::kQuietTicks);
  EXPECT_EQ(Releases(), 1);
}

TEST_F(IdleMemoryReleaserTest, WorkRestartsTheQuietPeriod) {
  IdleMemoryReleaser releaser(&CountRelease);
  releaser.Tick(true);
  QuietTicks(&releaser, IdleMemoryReleaser::kQuietTicks - 1);
  releaser.Tick(true);
  QuietTicks(&releaser, IdleMemoryReleaser::kQuietTicks - 1);
  EXPECT_EQ(Releases(), 0);
  releaser.Tick(false);
  EXPECT_EQ(Releases(), 1);
}

TEST_F(IdleMemoryReleaserTest, ReleasesAgainAfterMoreWork) {
  IdleMemoryReleaser releaser(&CountRelease);
  releaser.Tick(true);
  QuietTicks(&releaser, IdleMemoryReleaser::kQuietTicks);
  releaser.Tick(true);
  QuietTicks(&releaser, IdleMemoryReleaser::kQuietTicks);
  EXPECT_EQ(Releases(), 2);
}

TEST_F(IdleMemoryReleaserTest, NeverReleasesWithoutWork) {
  IdleMemoryReleaser releaser(&CountRelease);
  QuietTicks(&releaser, 5 * IdleMemoryReleaser::kQuietTicks);
  EXPECT_EQ(Releases(), 0);
}

TEST_F(IdleMemoryReleaserTest, DoesNothingWithoutAFunction) {
  IdleMemoryReleaser releaser(nullptr);
  releaser.Tick(true);
  QuietTicks(&releaser, IdleMemoryReleaser::kQuietTicks);
  EXPECT_EQ(Releases(), 0);
}
}  // namespace
}  // namespace redis_simple
