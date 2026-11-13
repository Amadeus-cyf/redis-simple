#include "utils/integers.h"

#include <gtest/gtest.h>

namespace redis_simple::utils {
TEST(IntegersTest, CountsDecimalDigitsAtBoundaries) {
  EXPECT_EQ(Digits10(0), 1);
  EXPECT_EQ(Digits10(9), 1);
  EXPECT_EQ(Digits10(10), 2);
  EXPECT_EQ(Digits10(99), 2);
  EXPECT_EQ(Digits10(100), 3);
  EXPECT_EQ(Digits10(999), 3);
  EXPECT_EQ(Digits10(1000), 4);
  EXPECT_EQ(Digits10(999999999999ULL), 12);
  EXPECT_EQ(Digits10(1000000000000ULL), 13);
  EXPECT_EQ(Digits10(18446744073709551615ULL), 20);
}
}  // namespace redis_simple::utils
