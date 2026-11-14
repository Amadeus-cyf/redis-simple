#include "tcp/tcp.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace redis_simple::tcp {
TEST(TcpTest, CreatedSocketIsNonblockingAndCloseOnExec) {
  const int fd = TcpCreateSocket(AF_INET, true);
  ASSERT_GE(fd, 0);

  const int status_flags = fcntl(fd, F_GETFL);
  const int descriptor_flags = fcntl(fd, F_GETFD);
  EXPECT_NE(status_flags, -1);
  EXPECT_NE(status_flags & O_NONBLOCK, 0);
  EXPECT_NE(descriptor_flags, -1);
  EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);

  close(fd);
}

TEST(TcpTest, AcceptedSocketSendsAtOnceAndProbesSilentPeers) {
  const int listener = TcpCreateSocket(AF_INET, false);
  ASSERT_GE(listener, 0);
  // Port 0: the kernel picks a free one.
  ASSERT_EQ(TcpBind(listener, TcpAddress("127.0.0.1", 0)), TcpStatusCode::kOk);
  ASSERT_EQ(TcpListen(listener), TcpStatusCode::kOk);
  sockaddr_in bound{};
  socklen_t bound_length = sizeof(bound);
  ASSERT_EQ(
      getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length),
      0);
  const int client = TcpBindAndConnect(
      TcpAddress("127.0.0.1", ntohs(bound.sin_port)), std::nullopt, false);
  ASSERT_GE(client, 0);

  TcpAddress peer;
  const int accepted = TcpAccept(listener, &peer);
  ASSERT_GE(accepted, 0);
  EXPECT_EQ(peer.ip, "127.0.0.1");
  const auto option = [accepted](int level, int name) {
    int value = 0;
    socklen_t length = sizeof(value);
    EXPECT_EQ(getsockopt(accepted, level, name, &value, &length), 0);
    return value;
  };
  EXPECT_NE(option(IPPROTO_TCP, TCP_NODELAY), 0);
  EXPECT_NE(option(SOL_SOCKET, SO_KEEPALIVE), 0);
  EXPECT_NE(fcntl(accepted, F_GETFL) & O_NONBLOCK, 0);

  close(accepted);
  close(client);
  close(listener);
}
}  // namespace redis_simple::tcp
