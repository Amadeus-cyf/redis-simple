#include <benchmark/benchmark.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "server/client.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "server/server_context.h"
#include "tcp/tcp.h"

// Client benchmarks: a request that reaches the client's socket in pieces,
// each read and parsed before the next arrives, as a large value does over a
// network.
namespace redis_simple {
namespace {
// The bytes the peer writes before each read: a segment of the loopback
// interface on macOS.
constexpr size_t kPieceBytes = size_t{16} * 1024;
// Room for a piece in the socket pair, which macOS creates with 8 KiB.
constexpr int kSocketBufferBytes = 256 * 1024;
constexpr std::string_view kKey = "client-benchmark-key";

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

bool SetSocketBufferSizes(int fd) {
  const int bytes = kSocketBufferBytes;
  return setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}

std::string EncodeRequest(const std::vector<std::string_view>& parts) {
  std::string request = reply::FromArrayHeader(parts.size());
  for (const auto part : parts) {
    reply::AppendBulkString(part, &request);
  }
  return request;
}

// One readable event: a read and a parse of what it brought.
bool ReadAndProcess(Client* const client) {
  return client->ReadQuery() > 0 &&
         client->ProcessInputBuffer() == ClientStatus::kOk;
}

// Writes `request` to `peer_fd` in pieces and has `client` read and parse
// each one, then sends the reply and takes it off the socket. False if the
// socket or the client fails.
bool DeliverInPieces(std::string_view request, int peer_fd,
                     Client* const client) {
  for (size_t offset = 0; offset < request.size(); offset += kPieceBytes) {
    const size_t size = std::min(kPieceBytes, request.size() - offset);
    if (write(peer_fd, request.data() + offset, size) !=
            static_cast<ssize_t>(size) ||
        !ReadAndProcess(client)) {
      return false;
    }
  }
  // A read may stop short of the piece's end. The event loop reports the
  // socket as readable again until it is empty.
  while (!client->HasPendingReplies()) {
    if (!ReadAndProcess(client)) {
      return false;
    }
  }
  std::array<char, 64> reply{};
  return client->SendReply() > 0 &&
         read(peer_fd, reply.data(), reply.size()) > 0;
}

// One SET of a value of range(0) bytes, delivered in pieces. With range(1)
// an option follows the value, so the value is not the request's last
// argument.
void ClientReadLargeSet(benchmark::State& state) {
  const std::string value(static_cast<size_t>(state.range(0)), 'v');
  std::vector<std::string_view> parts{"SET", kKey, value};
  if (state.range(1) != 0) {
    parts.emplace_back("KEEPTTL");
  }
  const std::string request = EncodeRequest(parts);

  const std::unique_ptr<db::RedisDb> db = db::RedisDb::Create();
  const ServerContext server{.db = db.get(),
                             .aof = nullptr,
                             .rdb = nullptr,
                             .changes = nullptr,
                             .required_password = {},
                             .io_threads = {},
                             .forks = nullptr};
  std::array<int, 2> fds{-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
    state.SkipWithError("socketpair failed");
    return;
  }
  ScopedFd client_fd(fds[0]);
  const ScopedFd peer_fd(fds[1]);
  if (!SetSocketBufferSizes(client_fd.Get()) ||
      !SetSocketBufferSizes(peer_fd.Get()) ||
      tcp::NonBlock(client_fd.Get()) != tcp::TcpStatusCode::kOk) {
    state.SkipWithError("could not set up the socket pair");
    return;
  }
  auto connection = std::make_unique<connection::Connection>(
      connection::Context{.loop = nullptr, .fd = client_fd.Release()});
  connection->SetState(connection::ConnectionState::kConnected);
  const std::unique_ptr<Client> client =
      Client::Create(std::move(connection), &server, OutputBufferLimits{});

  for (auto _ : state) {
    (void)_;
    if (!DeliverInPieces(request, peer_fd.Get(), client.get())) {
      state.SkipWithError("the request was not delivered");
      break;
    }
  }
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(request.size()));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ClientReadLargeSet)
    ->ArgsProduct({{int64_t{48} * 1024, int64_t{256} * 1024,
                    int64_t{1024} * 1024},
                   {0, 1}})
    ->ArgNames({"bytes", "option"});
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
