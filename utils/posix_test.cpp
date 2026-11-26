#include "utils/posix.h"

#include <gtest/gtest.h>
#include <sys/wait.h>

#include <cerrno>
#include <climits>

namespace redis_simple::utils {
namespace {
TEST(PosixTest, MaxIovCountIsAtLeastOneAndFitsAnInt) {
  EXPECT_GE(MaxIovCount(), 1);
  EXPECT_LE(MaxIovCount(), static_cast<size_t>(INT_MAX));
  EXPECT_EQ(MaxIovCount(), MaxIovCount());
}

TEST(PosixTest, WaitForChildReportsAMissingChild) {
  int wait_status = 0;
  errno = 0;
  EXPECT_EQ(WaitForChild(-1, &wait_status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}
}  // namespace
}  // namespace redis_simple::utils
