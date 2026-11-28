#include "server/server.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "server/aof/aof.h"
#include "server/aof/file_ops.h"
#include "server/server_options.h"
#include "utils/check.h"

namespace redis_simple {
namespace {
constexpr std::chrono::seconds kTimeout{10};
// Long enough for requests sent to reach the server, and for replies it
// sends to arrive.
constexpr std::chrono::milliseconds kArrivalTime{300};
constexpr std::string_view kOkReply = "+OK\r\n";
// More clients than the main thread runs, without an AOF, before it gives
// some back to their I/O thread.
constexpr size_t kLargeHandoff = 160;
// Enough for both ends of that many connections and the server's own files.
constexpr rlim_t kOpenFiles = 1024;

// The real file operations, with a sync the test can hold or fail.
class GatedFileOps final : public aof::FileOps {
 public:
  ssize_t Write(int fd, const void* data, size_t size) override {
    return real_->Write(fd, data, size);
  }
  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    return real_->WriteVector(fd, blocks, count);
  }
  int Sync(int fd) override {
    std::unique_lock lock(mutex_);
    ++sync_calls_;
    if (fail_syncs_) {
      errno = EIO;
      return -1;
    }
    if (hold_next_sync_) {
      hold_next_sync_ = false;
      holding_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return !holding_; });
    }
    lock.unlock();
    return real_->Sync(fd);
  }
  int Rename(const char* source, const char* destination) override {
    return real_->Rename(source, destination);
  }

  void HoldNextSync() {
    const std::scoped_lock lock(mutex_);
    hold_next_sync_ = true;
  }
  // False if no sync was held in time.
  bool WaitForHeldSync() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kTimeout, [this] { return holding_; });
  }
  void ReleaseSync() {
    const std::scoped_lock lock(mutex_);
    holding_ = false;
    changed_.notify_all();
  }
  void FailSyncs() {
    const std::scoped_lock lock(mutex_);
    fail_syncs_ = true;
  }
  int SyncCalls() {
    const std::scoped_lock lock(mutex_);
    return sync_calls_;
  }

 private:
  std::shared_ptr<aof::FileOps> real_{aof::DefaultFileOps()};
  std::mutex mutex_;
  std::condition_variable changed_;
  int sync_calls_{};
  bool hold_next_sync_{};
  bool holding_{};
  bool fail_syncs_{};
};

sockaddr_in LoopbackAddress(int port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

// A port nothing listens on, found by binding port 0.
int UnusedPort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  RS_CHECK(fd >= 0);
  sockaddr_in address = LoopbackAddress(0);
  socklen_t size = sizeof(address);
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  RS_CHECK(bind(fd, reinterpret_cast<sockaddr*>(&address), size) == 0);
  RS_CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  close(fd);
  return ntohs(address.sin_port);
}

// Raises the limit on open files to `count`, or to the hard limit if that is
// lower: the test and the server hold both ends of each connection in this
// process.
void AllowOpenFiles(rlim_t count) {
  rlimit limit{};
  RS_CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0);
  if (limit.rlim_cur < count) {
    limit.rlim_cur = std::min(count, limit.rlim_max);
    RS_CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
  }
}

// A blocking client socket whose reads give up after kTimeout.
class TestClient {
 public:
  // Retries while the server is still starting.
  explicit TestClient(int port) {
    const sockaddr_in address = LoopbackAddress(port);
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (fd_ < 0 && std::chrono::steady_clock::now() < deadline) {
      fd_ = socket(AF_INET, SOCK_STREAM, 0);
      RS_CHECK(fd_ >= 0);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      if (connect(fd_, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
        close(fd_);
        fd_ = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    RS_CHECK(fd_ >= 0);
    const timeval timeout{.tv_sec = kTimeout.count(), .tv_usec = 0};
    RS_CHECK(setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                        sizeof(timeout)) == 0);
  }
  TestClient(const TestClient&) = delete;
  TestClient& operator=(const TestClient&) = delete;
  TestClient(TestClient&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  TestClient& operator=(TestClient&&) = delete;
  ~TestClient() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  void Send(std::string_view request) const {
    RS_CHECK(send(fd_, request.data(), request.size(), 0) ==
             static_cast<ssize_t>(request.size()));
  }
  // The next `size` bytes, or fewer if the server closed the connection or
  // sent nothing in time.
  [[nodiscard]] std::string Receive(size_t size) const {
    std::string reply(size, '\0');
    size_t received = 0;
    while (received < size) {
      const ssize_t count =
          recv(fd_, reply.data() + received, size - received, 0);
      if (count <= 0) {
        break;
      }
      received += static_cast<size_t>(count);
    }
    reply.resize(received);
    return reply;
  }
  [[nodiscard]] bool HasReply() const {
    pollfd descriptor{.fd = fd_, .events = POLLIN, .revents = 0};
    return poll(&descriptor, 1, 0) > 0;
  }

 private:
  int fd_{-1};
};

// A server running on its own thread, with `appendfsync always` unless a
// test starts it without an AOF.
class ServerTest : public testing::Test {
 protected:
  void StartServer(int io_threads) {
    ServerOptions options = BaseOptions(io_threads);
    options.append_only = true;
    options.aof_options.directory = root_ + "/appendonlydir";
    options.aof_options.fsync = aof::FsyncPolicy::kAlways;
    options.aof_options.file_ops = file_ops_;
    Run(options);
  }

  void StartServerWithoutAof(int io_threads) {
    ServerOptions options = BaseOptions(io_threads);
    // The server loads the RDB file at start, and none is there.
    options.rdb_options.file_name = root_ + "/dump.rdb";
    Run(options);
  }

  // Connected clients the server has already answered once, so each is
  // registered with the event loop or its I/O thread.
  [[nodiscard]] std::vector<TestClient> ConnectClients(size_t count) const {
    std::vector<TestClient> clients;
    for (size_t index = 0; index < count; ++index) {
      clients.emplace_back(port_);
      clients.back().Send("PING\r\n");
      EXPECT_EQ(clients.back().Receive(7), "+PONG\r\n");
    }
    return clients;
  }

  // The server stops at its next cron run after SIGTERM, as when a user
  // signals the process. The signal goes to the server's thread, so the
  // handler's flag is written on the thread that reads it.
  void StopServer() {
    if (thread_.joinable()) {
      // The server's handler catches the signal; nothing is terminated.
      // NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread)
      pthread_kill(thread_.native_handle(), SIGTERM);
      thread_.join();
    }
  }

  // Waits for a server that stops by itself. Returns whether Run
  // reported success.
  bool JoinServer() {
    thread_.join();
    return run_succeeded_;
  }

  void HoldNextSync() { file_ops_->HoldNextSync(); }
  // False if no sync was held in time.
  [[nodiscard]] bool WaitForHeldSync() { return file_ops_->WaitForHeldSync(); }
  void ReleaseSync() { file_ops_->ReleaseSync(); }
  void FailSyncs() { file_ops_->FailSyncs(); }
  [[nodiscard]] bool JoinedServerSucceeded() const { return run_succeeded_; }

  void TearDown() override {
    file_ops_->ReleaseSync();
    StopServer();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  // One client's write is being synced while four more clients send theirs:
  // the first client has no reply until its sync ends, and the four share
  // the next sync.
  void ExpectOneSyncForClientsOfOneIteration() {
    constexpr size_t kLateClients = 4;
    const TestClient first = std::move(ConnectClients(1).front());
    const std::vector<TestClient> late = ConnectClients(kLateClients);
    const int syncs_before = file_ops_->SyncCalls();

    file_ops_->HoldNextSync();
    first.Send("SET first 1\r\n");
    ASSERT_TRUE(file_ops_->WaitForHeldSync());
    EXPECT_FALSE(first.HasReply());
    for (const TestClient& client : late) {
      client.Send("SET late 1\r\n");
    }
    // The requests reach the server, and with threaded I/O the main thread's
    // queue, while the main thread is still in the held sync.
    std::this_thread::sleep_for(kArrivalTime);
    EXPECT_FALSE(first.HasReply());
    file_ops_->ReleaseSync();

    EXPECT_EQ(first.Receive(kOkReply.size()), kOkReply);
    for (const TestClient& client : late) {
      EXPECT_EQ(client.Receive(kOkReply.size()), kOkReply);
    }
    EXPECT_EQ(file_ops_->SyncCalls() - syncs_before, 2);
  }

 private:
  // Options both servers share, with a new directory for their files.
  ServerOptions BaseOptions(int io_threads) {
    constexpr std::string_view kTemplate = "/tmp/redis_simple_server_XXXXXX";
    std::array<char, kTemplate.size() + 1> path{};
    std::ranges::copy(kTemplate, path.begin());
    RS_CHECK(mkdtemp(path.data()) != nullptr);
    root_ = path.data();
    ServerOptions options;
    options.port = UnusedPort();
    // No RDB file is saved at shutdown.
    options.rdb_options.save_points.clear();
    options.io_threads = io_threads;
    return options;
  }

  void Run(const ServerOptions& options) {
    port_ = options.port;
    thread_ = std::thread(
        [this, options] { run_succeeded_ = server_->Run(options); });
  }

  std::shared_ptr<GatedFileOps> file_ops_{std::make_shared<GatedFileOps>()};
  std::unique_ptr<Server> server_{Server::Create()};
  std::string root_;
  int port_{};
  std::thread thread_;
  bool run_succeeded_{};
};

TEST_F(ServerTest, SyncsTheAofOnceForTheClientsOfOneIteration) {
  StartServer(1);
  ExpectOneSyncForClientsOfOneIteration();
  StopServer();
  EXPECT_TRUE(JoinedServerSucceeded());
}

TEST_F(ServerTest, SyncsTheAofOnceForClientsHandedOverByIoThreads) {
  StartServer(4);
  ExpectOneSyncForClientsOfOneIteration();
  StopServer();
  EXPECT_TRUE(JoinedServerSucceeded());
}

TEST_F(ServerTest, NeverRepliesToAWriteTheAofCouldNotSync) {
  StartServer(1);
  const TestClient client = std::move(ConnectClients(1).front());
  FailSyncs();
  client.Send("SET key value\r\n");
  // The server stops and closes the connection without a reply.
  EXPECT_EQ(client.Receive(kOkReply.size()), "");
  EXPECT_TRUE(JoinServer());
}

TEST_F(ServerTest, NeverRepliesThroughAnIoThreadToAWriteTheAofCouldNotSync) {
  StartServer(4);
  const TestClient client = std::move(ConnectClients(1).front());
  FailSyncs();
  client.Send("SET key value\r\n");
  EXPECT_EQ(client.Receive(kOkReply.size()), "");
  EXPECT_TRUE(JoinServer());
}

TEST_F(ServerTest, HoldsRepliesOfALargeHandoffUntilTheAofSyncs) {
  AllowOpenFiles(kOpenFiles);
  // One I/O thread, so that the late clients come in one handoff.
  StartServer(2);
  const TestClient first = std::move(ConnectClients(1).front());
  const std::vector<TestClient> late = ConnectClients(kLargeHandoff);

  HoldNextSync();
  first.Send("SET first 1\r\n");
  ASSERT_TRUE(WaitForHeldSync());
  for (const TestClient& client : late) {
    client.Send("SET late 1\r\n");
  }
  std::this_thread::sleep_for(kArrivalTime);
  // The late clients' writes run after this sync, and the next one
  // persists them.
  HoldNextSync();
  ReleaseSync();
  EXPECT_EQ(first.Receive(kOkReply.size()), kOkReply);
  ASSERT_TRUE(WaitForHeldSync());
  std::this_thread::sleep_for(kArrivalTime);
  EXPECT_EQ(
      std::ranges::count_if(
          late, [](const TestClient& client) { return client.HasReply(); }),
      0);
  ReleaseSync();
  // Stops at the first client without a reply rather than wait for each.
  for (const TestClient& client : late) {
    ASSERT_EQ(client.Receive(kOkReply.size()), kOkReply);
  }
  StopServer();
  EXPECT_TRUE(JoinedServerSucceeded());
}

TEST_F(ServerTest, RepliesToEveryClientOfALargeHandoffWithoutAnAof) {
  constexpr size_t kPushes = 100;
  constexpr size_t kValuesPerPush = 1000;
  constexpr size_t kScans = 200;
  AllowOpenFiles(kOpenFiles);
  // One I/O thread, so that the clients come in one handoff.
  StartServerWithoutAof(2);
  const TestClient busy = std::move(ConnectClients(1).front());
  const std::vector<TestClient> clients = ConnectClients(kLargeHandoff);

  // A long list, so that each LREM below takes a while to scan it.
  std::string push = "RPUSH list";
  for (size_t value = 0; value < kValuesPerPush; ++value) {
    push += " x";
  }
  push += "\r\n";
  std::string pushes;
  std::string pushed;
  for (size_t index = 1; index <= kPushes; ++index) {
    pushes += push;
    pushed += ":" + std::to_string(index * kValuesPerPush) + "\r\n";
  }
  busy.Send(pushes);
  ASSERT_EQ(busy.Receive(pushed.size()), pushed);

  // Each client sets a key of its own and reads it back.
  const auto request = [](size_t index) {
    const std::string value = std::to_string(index);
    return "SET key" + value + " " + value + "\r\nGET key" + value + "\r\n";
  };
  const auto reply = [](size_t index) {
    const std::string value = std::to_string(index);
    return std::string(kOkReply) + "$" + std::to_string(value.size()) + "\r\n" +
           value + "\r\n";
  };
  // The main thread runs the scans while the clients' requests reach the I/O
  // thread, which hands them over together once the scans are done.
  std::string scans;
  std::string scanned;
  for (size_t index = 0; index < kScans; ++index) {
    scans += "LREM list 0 absent\r\n";
    scanned += ":0\r\n";
  }
  busy.Send(scans);
  for (size_t index = 0; index < clients.size(); ++index) {
    clients[index].Send(request(index));
  }
  // Stops at the first client without a reply rather than wait for each.
  for (size_t index = 0; index < clients.size(); ++index) {
    const std::string expected = reply(index);
    ASSERT_EQ(clients[index].Receive(expected.size()), expected);
  }
  EXPECT_EQ(busy.Receive(scanned.size()), scanned);
  StopServer();
  EXPECT_TRUE(JoinedServerSucceeded());
}
}  // namespace
}  // namespace redis_simple
