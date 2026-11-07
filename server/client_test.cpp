#include "server/client.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "server/aof.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "tcp/tcp.h"

namespace redis_simple {
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

db::RedisDb* TestDb() {
  // Leaked on purpose: static objects must be trivially destructible.
  static db::RedisDb* const kDb = db::RedisDb::Create().release();
  return kDb;
}

std::array<ScopedFd, 2> CreateSocketPair() {
  std::array<int, 2> fds{-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
  return {ScopedFd(fds[0]), ScopedFd(fds[1])};
}

std::pair<std::unique_ptr<Client>, ScopedFd> CreateClient(
    OutputBufferLimits limits = {}) {
  auto sockets = CreateSocketPair();
  EXPECT_EQ(tcp::NonBlock(sockets[0].Get()), tcp::TcpStatusCode::kOk);
  auto connection = std::make_unique<connection::Connection>(
      connection::Context{.loop = nullptr, .fd = sockets[0].Release()});
  connection->SetState(connection::ConnectionState::kConnected);
  return {Client::Create(std::move(connection), TestDb(), nullptr, limits),
          std::move(sockets[1])};
}

// File operations whose writes can be made to fail, as on a full disk.
class FailableFileOps final : public aof::FileOps {
 public:
  ssize_t Write(int fd, const void* data, size_t size) override {
    if (fail_writes.load()) {
      errno = ENOSPC;
      return -1;
    }
    return write(fd, data, size);
  }
  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    return writev(fd, blocks, count);
  }
  int Sync(int fd) override { return fsync(fd); }
  int Rename(const char* source, const char* destination) override {
    return std::rename(source, destination);
  }

  std::atomic<bool> fail_writes{false};
};

std::string EncodeRequest(const std::vector<std::string_view>& parts) {
  std::string request = reply::FromArrayHeader(parts.size());
  for (const auto part : parts) {
    reply::AppendBulkString(part, &request);
  }
  return request;
}

std::string SendAndReadReply(Client* client, int peer_fd) {
  while (client->HasPendingReplies()) {
    const ssize_t bytes_written = client->SendReply();
    EXPECT_GT(bytes_written, 0);
    if (bytes_written <= 0) {
      break;
    }
  }
  std::array<char, 4096> buffer{};
  const ssize_t bytes_read = read(peer_fd, buffer.data(), buffer.size());
  EXPECT_GT(bytes_read, 0);
  return bytes_read > 0
             ? std::string(buffer.data(), static_cast<size_t>(bytes_read))
             : std::string();
}
std::string Roundtrip(Client* client, int peer_fd, std::string_view request) {
  EXPECT_EQ(write(peer_fd, request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  return SendAndReadReply(client, peer_fd);
}
}  // namespace

TEST(ClientTest, ReadQueryReadsOncePerCall) {
  auto sockets = CreateSocketPair();
  ASSERT_EQ(tcp::NonBlock(sockets[0].Get()), tcp::TcpStatusCode::kOk);
  auto connection = std::make_unique<connection::Connection>(
      connection::Context{.loop = nullptr, .fd = sockets[0].Release()});
  connection->SetState(connection::ConnectionState::kConnected);
  auto client = Client::Create(std::move(connection), TestDb(), nullptr);

  const std::string query(8192, 'x');
  ASSERT_EQ(write(sockets[1].Get(), query.data(), query.size()),
            static_cast<ssize_t>(query.size()));

  EXPECT_EQ(client->ReadQuery(), static_cast<ssize_t>(query.size()));
  EXPECT_EQ(client->ReadQuery(), 0);
  EXPECT_EQ(client->Connection()->State(),
            connection::ConnectionState::kConnected);
}

TEST(ClientTest, ProcessesFragmentedRespRequest) {
  auto [client, peer] = CreateClient();
  constexpr std::string_view kFirst = "*2\r\n$3\r\nGET\r\n$9\r\nmis";
  constexpr std::string_view kSecond = "sing-1\r\n";

  ASSERT_EQ(write(peer.Get(), kFirst.data(), kFirst.size()),
            static_cast<ssize_t>(kFirst.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(kFirst.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  EXPECT_FALSE(client->HasPendingReplies());

  ASSERT_EQ(write(peer.Get(), kSecond.data(), kSecond.size()),
            static_cast<ssize_t>(kSecond.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(kSecond.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()), "$-1\r\n");
}

TEST(ClientTest, ResumesLargeRequestAcrossBufferGrowth) {
  auto [client, peer] = CreateClient();
  const std::string value(60000, 'v');
  const std::string request = EncodeRequest({"SET", "large-key", value}) +
                              EncodeRequest({"GET", "large-key"});

  // Deliver the request in pieces so the query buffer grows and compacts
  // while the parser holds partial state.
  constexpr size_t kPieceSize = 7000;
  for (size_t offset = 0; offset < request.size(); offset += kPieceSize) {
    const size_t size = std::min(kPieceSize, request.size() - offset);
    ASSERT_EQ(write(peer.Get(), request.data() + offset, size),
              static_cast<ssize_t>(size));
    ASSERT_GT(client->ReadQuery(), 0);
    ASSERT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  }

  std::string expected = "+OK\r\n$60000\r\n";
  expected.append(value).append("\r\n");
  // Interleave sends and reads: the reply is larger than some platforms'
  // socket buffers (about 8 KiB for macOS socket pairs).
  std::string response;
  std::array<char, 4096> buffer{};
  while (response.size() < expected.size()) {
    if (client->HasPendingReplies()) {
      // A full socket buffer makes this a would-block, not an error.
      client->SendReply();
      ASSERT_EQ(client->Connection()->State(),
                connection::ConnectionState::kConnected);
    }
    const ssize_t bytes_read = read(peer.Get(), buffer.data(), buffer.size());
    ASSERT_GT(bytes_read, 0);
    response.append(buffer.data(), static_cast<size_t>(bytes_read));
  }
  EXPECT_EQ(response, expected);
  client->Db()->DeleteKey("large-key");
}

TEST(ClientTest, RespRequestsPreserveSpacesAndBinaryData) {
  auto [client, peer] = CreateClient();
  const std::string key = "client test key";
  const std::string value("hello\0world", 11);
  const std::string request =
      EncodeRequest({"SET", key, value}) + EncodeRequest({"GET", key});

  ASSERT_EQ(write(peer.Get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);

  std::string expected = "+OK\r\n$11\r\n";
  expected.append(value).append("\r\n");
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()), expected);
  client->Db()->DeleteKey(key);
}

TEST(ClientTest, HelloNegotiatesResp3) {
  auto [client, peer] = CreateClient();
  const std::string request = EncodeRequest({"HELLO", "3"}) +
                              EncodeRequest({"GET", "missing-resp3-key"}) +
                              EncodeRequest({"HGETALL", "missing-resp3-hash"}) +
                              EncodeRequest({"SMEMBERS", "missing-resp3-set"});

  ASSERT_EQ(write(peer.Get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);

  const std::string response = SendAndReadReply(client.get(), peer.Get());
  EXPECT_EQ(client->Protocol(), reply::ProtocolVersion::kResp3);
  EXPECT_EQ(response.rfind("%7\r\n", 0), 0);
  EXPECT_EQ(response.substr(response.size() - 11), "_\r\n%0\r\n~0\r\n");
}

TEST(ClientTest, HandlesConnectionCommandsAndStopsAfterQuit) {
  auto [client, peer] = CreateClient();
  const std::string payload("hello\0world", 11);
  const std::string request = EncodeRequest({"PING"}) +
                              EncodeRequest({"PING", payload}) +
                              EncodeRequest({"ECHO", payload}) +
                              EncodeRequest({"QUIT"}) + EncodeRequest({"PING"});

  ASSERT_EQ(write(peer.Get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  EXPECT_TRUE(client->ShouldCloseAfterReply());

  std::string expected = "+PONG\r\n$11\r\n";
  expected.append(payload).append("\r\n$11\r\n").append(payload);
  expected.append("\r\n+OK\r\n");
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()), expected);
}

TEST(ClientTest, ProtocolErrorsCloseAfterReply) {
  auto [client, peer] = CreateClient();
  constexpr std::string_view kRequest = "*1\r\n+GET\r\n";

  ASSERT_EQ(write(peer.Get(), kRequest.data(), kRequest.size()),
            static_cast<ssize_t>(kRequest.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(kRequest.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  EXPECT_TRUE(client->ShouldCloseAfterReply());
  EXPECT_TRUE(client->ShouldPauseReads());
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()),
            "-ERR Protocol error: expected '$', got '+'\r\n");
}

TEST(ClientTest, RejectsInvalidArityBeforeDispatch) {
  auto [client, peer] = CreateClient();
  const std::string request =
      EncodeRequest(std::vector<std::string_view>{"GET"}) +
      EncodeRequest(std::vector<std::string_view>{"PING"});

  ASSERT_EQ(write(peer.Get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()),
            "-ERR wrong number of arguments for 'get' command\r\n+PONG\r\n");
}

TEST(ClientTest, RequiresAuthWhenPasswordIsSet) {
  auto [client, peer] = CreateClient();
  client->RequirePassword("s3cret");
  EXPECT_FALSE(client->Authenticated());
  const std::string request =
      EncodeRequest({"PING"}) + EncodeRequest({"GET", "key"}) +
      EncodeRequest({"AUTH", "wrong"}) +
      EncodeRequest({"AUTH", "other", "s3cret"}) + EncodeRequest({"PING"}) +
      EncodeRequest({"AUTH", "s3cret"}) + EncodeRequest({"PING"}) +
      EncodeRequest({"AUTH", "default", "s3cret"});

  ASSERT_EQ(write(peer.Get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  EXPECT_TRUE(client->Authenticated());

  constexpr std::string_view kNoAuth = "-NOAUTH Authentication required.\r\n";
  constexpr std::string_view kWrongPass =
      "-WRONGPASS invalid username-password pair or user is disabled.\r\n";
  std::string expected;
  expected.append(kNoAuth).append(kNoAuth);
  expected.append(kWrongPass).append(kWrongPass).append(kNoAuth);
  expected.append("+OK\r\n+PONG\r\n+OK\r\n");
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()), expected);
}

TEST(ClientTest, QuitWorksBeforeAuth) {
  auto [client, peer] = CreateClient();
  client->RequirePassword("s3cret");
  const std::string request = EncodeRequest({"QUIT"});

  ASSERT_EQ(write(peer.Get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  EXPECT_TRUE(client->ShouldCloseAfterReply());
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()), "+OK\r\n");
}

TEST(ClientTest, AuthWithoutConfiguredPasswordFails) {
  auto [client, peer] = CreateClient();
  EXPECT_TRUE(client->Authenticated());
  const std::string request =
      EncodeRequest({"AUTH", "anything"}) + EncodeRequest({"PING"});

  ASSERT_EQ(write(peer.Get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(client->ReadQuery(), static_cast<ssize_t>(request.size()));
  EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
  const std::string reply = SendAndReadReply(client.get(), peer.Get());
  EXPECT_EQ(reply.rfind("-ERR AUTH <password> called without any password", 0),
            0);
  EXPECT_NE(reply.find("\r\n+PONG\r\n"), std::string::npos);
}

TEST(ClientTest, OutputLimitsControlBackpressure) {
  auto [client, peer] =
      CreateClient({.resume_bytes = 4, .soft_bytes = 8, .hard_bytes = 16});
  EXPECT_EQ(client->AddReply(std::string_view("12345678")), 8);
  EXPECT_TRUE(client->ShouldPauseReads());
  client->SetReadsPaused(true);

  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()), "12345678");
  EXPECT_TRUE(client->ShouldResumeReads());

  auto [limited_client, limited_peer] =
      CreateClient({.resume_bytes = 4, .soft_bytes = 8, .hard_bytes = 16});
  (void)limited_peer;
  EXPECT_EQ(limited_client->AddReply(std::string(17, 'x')), 0);
  EXPECT_TRUE(limited_client->ShouldCloseAfterReply());
  EXPECT_FALSE(limited_client->HasPendingReplies());
}

TEST(ClientTest, QueuesSplitReplyAtomically) {
  auto [client, peer] =
      CreateClient({.resume_bytes = 4, .soft_bytes = 8, .hard_bytes = 16});
  EXPECT_EQ(client->AddReply(std::string("header"), std::string("body")), 10);
  EXPECT_EQ(SendAndReadReply(client.get(), peer.Get()), "headerbody");

  auto [limited_client, limited_peer] =
      CreateClient({.resume_bytes = 4, .soft_bytes = 8, .hard_bytes = 16});
  (void)limited_peer;
  EXPECT_EQ(limited_client->AddReply(std::string(8, 'h'), std::string(9, 'b')),
            0);
  EXPECT_TRUE(limited_client->ShouldCloseAfterReply());
  EXPECT_FALSE(limited_client->HasPendingReplies());
}

TEST(ClientTest, ExecRunsQueuedCommandsInOrder) {
  auto [client, peer] = CreateClient();
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"MULTI"}) +
                          EncodeRequest({"SET", "tx-key", "1"}) +
                          EncodeRequest({"INCR", "tx-key"})),
            "+OK\r\n+QUEUED\r\n+QUEUED\r\n");
  EXPECT_TRUE(client->InTransaction());
  // Commands are not applied until EXEC.
  EXPECT_EQ(client->Db()->LookupKey("tx-key"), nullptr);

  // Queued arguments must survive later reads reusing the query buffer.
  EXPECT_EQ(
      Roundtrip(client.get(), peer.Get(),
                EncodeRequest({"GET", "tx-key"}) + EncodeRequest({"EXEC"})),
      "+QUEUED\r\n*3\r\n+OK\r\n:2\r\n$1\r\n2\r\n");
  EXPECT_FALSE(client->InTransaction());
  client->Db()->DeleteKey("tx-key");
}

TEST(ClientTest, ExecReportsRuntimeErrorsAndKeepsGoing) {
  auto [client, peer] = CreateClient();
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"SET", "tx-string", "text"}) +
                          EncodeRequest({"MULTI"}) +
                          EncodeRequest({"LPUSH", "tx-string", "x"}) +
                          EncodeRequest({"SET", "tx-after", "1"}) +
                          EncodeRequest({"EXEC"})),
            std::string("+OK\r\n+OK\r\n+QUEUED\r\n+QUEUED\r\n*2\r\n")
                .append(reply::kWrongTypeError)
                .append("+OK\r\n"));
  EXPECT_NE(client->Db()->LookupKey("tx-after"), nullptr);
  client->Db()->DeleteKey("tx-string");
  client->Db()->DeleteKey("tx-after");
}

TEST(ClientTest, RejectedCommandsAbortTransaction) {
  auto [client, peer] = CreateClient();
  EXPECT_EQ(
      Roundtrip(client.get(), peer.Get(),
                EncodeRequest({"MULTI"}) +
                    EncodeRequest({"SET", "tx-aborted", "1"}) +
                    EncodeRequest({"GET"}) + EncodeRequest({"EXEC"})),
      "+OK\r\n+QUEUED\r\n-ERR wrong number of arguments for 'get' command\r\n"
      "-EXECABORT Transaction discarded because of previous "
      "errors.\r\n");
  EXPECT_FALSE(client->InTransaction());
  EXPECT_EQ(client->Db()->LookupKey("tx-aborted"), nullptr);

  EXPECT_EQ(
      Roundtrip(client.get(), peer.Get(),
                EncodeRequest({"MULTI"}) + EncodeRequest({"NOSUCH"}) +
                    EncodeRequest({"EXEC"})),
      "+OK\r\n-ERR unknown command 'NOSUCH', with args beginning with: \r\n"
      "-EXECABORT Transaction discarded because of previous "
      "errors.\r\n");
}

TEST(ClientTest, TransactionCommandsValidateState) {
  auto [client, peer] = CreateClient();
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"EXEC"}) + EncodeRequest({"DISCARD"}) +
                          EncodeRequest({"MULTI"}) + EncodeRequest({"MULTI"}) +
                          EncodeRequest({"WATCH", "key"}) +
                          EncodeRequest({"SET", "tx-discarded", "1"}) +
                          EncodeRequest({"DISCARD"}) + EncodeRequest({"EXEC"})),
            "-ERR EXEC without MULTI\r\n-ERR DISCARD without MULTI\r\n"
            "+OK\r\n-ERR MULTI calls can not be nested\r\n"
            "-ERR WATCH inside MULTI is not allowed\r\n+QUEUED\r\n"
            "+OK\r\n-ERR EXEC without MULTI\r\n");
  EXPECT_EQ(client->Db()->LookupKey("tx-discarded"), nullptr);
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"MULTI"}) + EncodeRequest({"EXEC"})),
            "+OK\r\n*0\r\n");
}

TEST(ClientTest, WatchAbortsExecWhenAnotherClientWrites) {
  auto [watching, watching_peer] = CreateClient();
  auto [writer, writer_peer] = CreateClient();
  EXPECT_EQ(Roundtrip(writer.get(), writer_peer.Get(),
                      EncodeRequest({"RPUSH", "tx-watched", "a"})),
            ":1\r\n");
  EXPECT_EQ(Roundtrip(watching.get(), watching_peer.Get(),
                      EncodeRequest({"WATCH", "tx-watched"})),
            "+OK\r\n");

  // A failed write does not modify the key.
  EXPECT_EQ(Roundtrip(writer.get(), writer_peer.Get(),
                      EncodeRequest({"INCR", "tx-watched"})),
            reply::kWrongTypeError);
  EXPECT_EQ(Roundtrip(watching.get(), watching_peer.Get(),
                      EncodeRequest({"MULTI"}) +
                          EncodeRequest({"LLEN", "tx-watched"}) +
                          EncodeRequest({"EXEC"})),
            "+OK\r\n+QUEUED\r\n*1\r\n:1\r\n");

  // EXEC unwatched the key; watch again, then edit the list in place.
  EXPECT_EQ(Roundtrip(watching.get(), watching_peer.Get(),
                      EncodeRequest({"WATCH", "tx-watched"})),
            "+OK\r\n");
  EXPECT_EQ(Roundtrip(writer.get(), writer_peer.Get(),
                      EncodeRequest({"RPUSH", "tx-watched", "b"})),
            ":2\r\n");
  EXPECT_EQ(Roundtrip(watching.get(), watching_peer.Get(),
                      EncodeRequest({"MULTI"}) +
                          EncodeRequest({"RPUSH", "tx-watched", "c"}) +
                          EncodeRequest({"EXEC"})),
            "+OK\r\n+QUEUED\r\n*-1\r\n");
  EXPECT_EQ(Roundtrip(writer.get(), writer_peer.Get(),
                      EncodeRequest({"LLEN", "tx-watched"})),
            ":2\r\n");
  EXPECT_FALSE(watching->Db()->HasWatchedKeys());
  watching->Db()->DeleteKey("tx-watched");
}

TEST(ClientTest, UnwatchAndDisconnectReleaseWatchedKeys) {
  auto [client, peer] = CreateClient();
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"HELLO", "3"}) +
                          EncodeRequest({"WATCH", "tx-a", "tx-b"}) +
                          EncodeRequest({"SET", "tx-b", "1"}) +
                          EncodeRequest({"MULTI"}) + EncodeRequest({"EXEC"}))
                .substr(0, 4),
            "%7\r\n");
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"WATCH", "tx-a"}) +
                          EncodeRequest({"SET", "tx-a", "1"}) +
                          EncodeRequest({"MULTI"}) + EncodeRequest({"EXEC"})),
            "+OK\r\n+OK\r\n+OK\r\n_\r\n");
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"WATCH", "tx-a"}) +
                          EncodeRequest({"SET", "tx-a", "2"}) +
                          EncodeRequest({"UNWATCH"}) +
                          EncodeRequest({"MULTI"}) + EncodeRequest({"EXEC"})),
            "+OK\r\n+OK\r\n+OK\r\n+OK\r\n*0\r\n");

  EXPECT_EQ(
      Roundtrip(client.get(), peer.Get(), EncodeRequest({"WATCH", "tx-a"})),
      "+OK\r\n");
  EXPECT_TRUE(client->Db()->HasWatchedKeys());
  db::RedisDb* const redis_db = client->Db();
  client.reset();
  EXPECT_FALSE(redis_db->HasWatchedKeys());
  redis_db->DeleteKey("tx-a");
  redis_db->DeleteKey("tx-b");
}

TEST(ClientTest, SetOverwritesStringsInPlaceAndReplacesOtherTypes) {
  auto [client, peer] = CreateClient();
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"SET", "overwrite-key", "first"}) +
                          EncodeRequest({"SET", "overwrite-key", "2"}) +
                          EncodeRequest({"INCR", "overwrite-key"}) +
                          EncodeRequest({"MSET", "overwrite-key", "third",
                                         "overwrite-list", "x"}) +
                          EncodeRequest({"GET", "overwrite-key"})),
            "+OK\r\n+OK\r\n:3\r\n+OK\r\n$5\r\nthird\r\n");
  // SET replaces a key holding another type with a string.
  EXPECT_EQ(Roundtrip(client.get(), peer.Get(),
                      EncodeRequest({"DEL", "overwrite-list"}) +
                          EncodeRequest({"RPUSH", "overwrite-list", "a"}) +
                          EncodeRequest({"SET", "overwrite-list", "text"}) +
                          EncodeRequest({"TYPE", "overwrite-list"}) +
                          EncodeRequest({"GET", "overwrite-list"})),
            ":1\r\n:1\r\n+OK\r\n+string\r\n$4\r\ntext\r\n");
  client->Db()->DeleteKey("overwrite-key");
  client->Db()->DeleteKey("overwrite-list");
}
TEST(ClientTest, RejectsWritesWithMisconfWhileTheAofCannotWrite) {
  std::string path = "/tmp/redis_simple_client_aof.XXXXXX";
  ASSERT_NE(mkdtemp(path.data()), nullptr);
  auto file_ops = std::make_shared<FailableFileOps>();
  auto db = db::RedisDb::Create();
  aof::Options options{path + "/appendonlydir", "appendonly.aof",
                       aof::FsyncPolicy::kEverySecond};
  options.file_ops = file_ops;
  auto aof = aof::Aof::Open(options, db.get());
  ASSERT_NE(aof, nullptr);

  auto sockets = CreateSocketPair();
  ASSERT_EQ(tcp::NonBlock(sockets[0].Get()), tcp::TcpStatusCode::kOk);
  auto connection = std::make_unique<connection::Connection>(
      connection::Context{.loop = nullptr, .fd = sockets[0].Release()});
  connection->SetState(connection::ConnectionState::kConnected);
  auto client = Client::Create(std::move(connection), db.get(), aof.get());
  const int peer = sockets[1].Get();

  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"SET", "key", "v"})),
            "+OK\r\n");
  // The server flushes after each read; this flush fails, as on a full disk.
  // Without `always` the reply already sent stands: the bytes are kept and
  // retried.
  file_ops->fail_writes.store(true);
  ASSERT_TRUE(aof->Flush());
  ASSERT_FALSE(aof->AcceptingWrites());

  const std::string misconf =
      "-MISCONF Errors writing to the AOF file: No space left on device\r\n";
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"SET", "key", "w"})),
            misconf);
  // Reads keep working.
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"GET", "key"})),
            "$1\r\nv\r\n");
  // A transaction with a write aborts, whether the write was rejected while
  // queueing or the AOF failed after it was queued.
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"MULTI"})), "+OK\r\n");
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"SET", "key", "w"})),
            misconf);
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"EXEC"})),
            "-EXECABORT Transaction discarded because of previous errors.\r\n");

  // The retried write succeeds once the disk has room again.
  file_ops->fail_writes.store(false);
  ASSERT_TRUE(aof->Flush());
  EXPECT_TRUE(aof->AcceptingWrites());
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"SET", "key", "w"})),
            "+OK\r\n");
  client.reset();
  aof.reset();
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

TEST(ClientTest, ExecRejectsQueuedWritesWhenTheAofFailsAfterQueueing) {
  std::string path = "/tmp/redis_simple_client_aof.XXXXXX";
  ASSERT_NE(mkdtemp(path.data()), nullptr);
  auto file_ops = std::make_shared<FailableFileOps>();
  auto db = db::RedisDb::Create();
  aof::Options options{path + "/appendonlydir", "appendonly.aof",
                       aof::FsyncPolicy::kEverySecond};
  options.file_ops = file_ops;
  auto aof = aof::Aof::Open(options, db.get());
  ASSERT_NE(aof, nullptr);
  auto sockets = CreateSocketPair();
  ASSERT_EQ(tcp::NonBlock(sockets[0].Get()), tcp::TcpStatusCode::kOk);
  auto connection = std::make_unique<connection::Connection>(
      connection::Context{.loop = nullptr, .fd = sockets[0].Release()});
  connection->SetState(connection::ConnectionState::kConnected);
  auto client = Client::Create(std::move(connection), db.get(), aof.get());
  const int peer = sockets[1].Get();

  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"MULTI"})), "+OK\r\n");
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"SET", "key", "v"})),
            "+QUEUED\r\n");
  // Another client's write fails to reach the file.
  file_ops->fail_writes.store(true);
  ASSERT_TRUE(aof->Append("SET", {"key", "v"}, db.get()));
  ASSERT_TRUE(aof->Flush());
  EXPECT_EQ(
      Roundtrip(client.get(), peer, EncodeRequest({"EXEC"})),
      "-MISCONF Errors writing to the AOF file: No space left on device\r\n");
  EXPECT_EQ(Roundtrip(client.get(), peer, EncodeRequest({"GET", "key"})),
            "$-1\r\n");
  file_ops->fail_writes.store(false);
  client.reset();
  aof.reset();
  std::error_code error;
  std::filesystem::remove_all(path, error);
}
}  // namespace redis_simple
