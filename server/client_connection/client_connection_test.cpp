#include "server/client_connection/client_connection.h"

#include <gtest/gtest.h>

namespace redis_simple::client_connection {
TEST(ProtectedModeTest, RejectsRemoteClientsWithoutPassword) {
  EXPECT_TRUE(RejectedByProtectedMode(true, "", "10.0.0.5"));
  EXPECT_TRUE(RejectedByProtectedMode(true, "", "192.168.1.20"));
  EXPECT_TRUE(RejectedByProtectedMode(true, "", "0.0.0.0"));
}

TEST(ProtectedModeTest, AcceptsLoopbackClients) {
  EXPECT_FALSE(RejectedByProtectedMode(true, "", "127.0.0.1"));
  EXPECT_FALSE(RejectedByProtectedMode(true, "", "127.10.0.1"));
}

TEST(ProtectedModeTest, PasswordOrDisabledModeAllowsRemoteClients) {
  EXPECT_FALSE(RejectedByProtectedMode(true, "s3cret", "10.0.0.5"));
  EXPECT_FALSE(RejectedByProtectedMode(false, "", "10.0.0.5"));
}
}  // namespace redis_simple::client_connection
