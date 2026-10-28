#include "connection/connection.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <future>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "tcp/tcp.h"

namespace redis_simple::connection {
namespace {
class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  ScopedFd& operator=(ScopedFd&&) = delete;
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  [[nodiscard]] int Get() const { return fd_; }
  int Release() { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

std::array<ScopedFd, 2> CreateSocketPair() {
  std::array<int, 2> fds{-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
  return {ScopedFd(fds[0]), ScopedFd(fds[1])};
}
}  // namespace

static_assert(!std::is_copy_constructible_v<Connection>);
static_assert(!std::is_copy_assignable_v<Connection>);
static_assert(!std::is_move_constructible_v<Connection>);
static_assert(!std::is_move_assignable_v<Connection>);

TEST(ConnectionTest, FailedAcceptDoesNotCloseListener) {
  ScopedFd listener(tcp::TcpCreateSocket(AF_INET, true));
  ASSERT_GE(listener.Get(), 0);
  ASSERT_EQ(tcp::TcpBind(listener.Get(), {"127.0.0.1", 0}),
            tcp::TcpStatusCode::kOk);
  ASSERT_EQ(tcp::TcpListen(listener.Get()), tcp::TcpStatusCode::kOk);

  Connection connection({nullptr, -1});
  connection.SetState(ConnectionState::kAccepting);
  EXPECT_EQ(connection.Accept(listener.Get(), nullptr),
            ConnectionStatus::kError);
  EXPECT_NE(fcntl(listener.Get(), F_GETFD), -1);
}

TEST(ConnectionTest, CallbackCanCloseConnection) {
  auto loop = event_loop::Loop::Create();
  ASSERT_NE(loop, nullptr);
  auto sockets = CreateSocketPair();
  Connection connection({loop.get(), sockets[0].Release()});
  connection.SetState(ConnectionState::kConnected);
  int callback_count = 0;
  ASSERT_TRUE(connection.SetReadCallback([&callback_count](Connection* conn) {
    char byte = '\0';
    EXPECT_EQ(conn->Read({&byte, 1}), 1);
    ++callback_count;
    conn->Close();
  }));
  ASSERT_EQ(write(sockets[1].Get(), "x", 1), 1);

  loop->ProcessEvents();

  EXPECT_EQ(callback_count, 1);
  EXPECT_EQ(connection.Descriptor(), -1);
  EXPECT_EQ(connection.State(), ConnectionState::kClosed);
}

TEST(ConnectionTest, SyncWriteCompletesAfterPartialWrites) {
  auto sockets = CreateSocketPair();
  ASSERT_EQ(tcp::NonBlock(sockets[0].Get()), tcp::TcpStatusCode::kOk);
  int send_buffer_size = 1024;
  ASSERT_EQ(setsockopt(sockets[0].Get(), SOL_SOCKET, SO_SNDBUF,
                       &send_buffer_size, sizeof(send_buffer_size)),
            0);
  Connection connection({nullptr, sockets[0].Release()});
  connection.SetState(ConnectionState::kConnected);
  const std::string payload(size_t{256} * 1024, 'x');
  auto reader = std::async(
      std::launch::async, [&peer = sockets[1], expected = payload.size()] {
        std::array<char, 4096> buffer{};
        size_t total = 0;
        while (total < expected) {
          const ssize_t count = read(peer.Get(), buffer.data(),
                                     std::min(buffer.size(), expected - total));
          if (count <= 0) {
            break;
          }
          total += static_cast<size_t>(count);
        }
        return total;
      });

  EXPECT_EQ(connection.SyncWrite(payload, 1000),
            static_cast<ssize_t>(payload.size()));
  EXPECT_EQ(reader.get(), payload.size());
}

TEST(ConnectionTest, WriteBarrierRunsCallbacksOnceInWriteFirstOrder) {
  auto loop = event_loop::Loop::Create();
  ASSERT_NE(loop, nullptr);
  auto sockets = CreateSocketPair();
  Connection connection({loop.get(), sockets[0].Release()});
  connection.SetState(ConnectionState::kConnected);
  std::vector<int> order;
  ASSERT_TRUE(connection.SetReadCallback([&order](Connection* conn) {
    char byte = '\0';
    EXPECT_EQ(conn->Read({&byte, 1}), 1);
    order.push_back(2);
  }));
  ASSERT_TRUE(connection.SetWriteCallback(
      [&order](Connection*) { order.push_back(1); }, true));
  ASSERT_EQ(write(sockets[1].Get(), "x", 1), 1);

  loop->ProcessEvents();

  EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(ConnectionTest, WriteVectorSendsAllBlocks) {
  auto sockets = CreateSocketPair();
  Connection connection({nullptr, sockets[0].Release()});
  connection.SetState(ConnectionState::kConnected);
  std::array<std::string, 3> parts{"first", "-second", "-third"};
  std::vector<iovec> blocks;
  blocks.reserve(parts.size());
  for (auto& part : parts) {
    blocks.push_back({part.data(), part.size()});
  }

  EXPECT_EQ(connection.WriteVector(blocks), 18);

  std::array<char, 18> received{};
  EXPECT_EQ(read(sockets[1].Get(), received.data(), received.size()), 18);
  EXPECT_EQ(std::string_view(received.data(), received.size()),
            "first-second-third");
}
}  // namespace redis_simple::connection
