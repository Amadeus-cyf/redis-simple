#include "utils/size_class.h"

#include <gtest/gtest.h>

#include <cstddef>

namespace redis_simple::utils {
TEST(SizeClassTest, StepsByEightBytesUpToSixtyFour) {
  EXPECT_EQ(AllocationSizeClass(0), 8);
  EXPECT_EQ(AllocationSizeClass(1), 8);
  EXPECT_EQ(AllocationSizeClass(8), 8);
  EXPECT_EQ(AllocationSizeClass(9), 16);
  EXPECT_EQ(AllocationSizeClass(57), 64);
  EXPECT_EQ(AllocationSizeClass(64), 64);
}

TEST(SizeClassTest, HasFourClassesPerPowerOfTwo) {
  EXPECT_EQ(AllocationSizeClass(65), 80);
  EXPECT_EQ(AllocationSizeClass(80), 80);
  EXPECT_EQ(AllocationSizeClass(81), 96);
  EXPECT_EQ(AllocationSizeClass(113), 128);
  EXPECT_EQ(AllocationSizeClass(128), 128);
  EXPECT_EQ(AllocationSizeClass(129), 160);
  EXPECT_EQ(AllocationSizeClass(8193), 10240);
  EXPECT_EQ(AllocationSizeClass(57345), 65536);
  EXPECT_EQ(AllocationSizeClass(65536), 65536);
}

TEST(SizeClassTest, LeavesLargeRequestsUnchanged) {
  EXPECT_EQ(AllocationSizeClass(65537), 65537);
  EXPECT_EQ(AllocationSizeClass(size_t{1} << 30), size_t{1} << 30);
}

TEST(SizeClassTest, NeverShrinksAndIsItsOwnClass) {
  for (size_t bytes = 0; bytes <= 70000; ++bytes) {
    const size_t size_class = AllocationSizeClass(bytes);
    ASSERT_GE(size_class, bytes);
    ASSERT_EQ(AllocationSizeClass(size_class), size_class);
  }
}
}  // namespace redis_simple::utils
