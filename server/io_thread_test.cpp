#include "server/io_thread.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "event_loop/loop.h"
#include "server/client.h"
#include "server/db/db.h"
#include "tcp/tcp.h"

namespace redis_simple {
namespace {
using Handoff = IoThread::Handoff;

// Plays the server's main thread: runs handed-over clients' commands and
// returns them, or records closed ones.
class MainThread {
 public:
  MainThread()
      : loop_(event_loop::Loop::Create()), db_(db::RedisDb::Create()) {}
  MainThread(const MainThread&) = delete;
  MainThread& operator=(const MainThread&) = delete;
  MainThread(MainThread&&) = delete;
  MainThread& operator=(MainThread&&) = delete;

  std::unique_ptr<IoThread> StartThread(
      const std::atomic<bool>* main_running = nullptr) {
    return IoThread::Create(
        1, loop_.get(),
        [this](IoThread* const thread, std::vector<Handoff>* const handoffs) {
          ++handoff_batches_;
          std::vector<Client*> ran;
          for (const Handoff& handoff : *handoffs) {
            if (handoff.closed) {
              closed_.push_back(handoff.client);
              continue;
            }
            if (hold_) {
              held_thread_ = thread;
              held_.push_back(handoff.client);
              continue;
            }
            handoff.client->RunParsedQueries();
            ran.push_back(handoff.client);
          }
          thread->Return(ran);
        },
        main_running);
  }

  // Starts a thread that wakes this one only while it sleeps, as the
  // server's does: the loop takes the thread's handoffs before it sleeps,
  // says it sleeps, and takes them once more (Server::BeforeSleep).
  std::unique_ptr<IoThread> StartThreadThatSkipsWakeups() {
    std::unique_ptr<IoThread> thread = StartThread(&running_);
    IoThread* const instance = thread.get();
    loop_->SetBeforeSleep([this, instance] {
      if (!instance->TakeHandoffs()) {
        running_.store(false);
        instance->TakeHandoffs();
      }
    });
    loop_->SetAfterSleep([this] { running_.store(true); });
    return thread;
  }

  // Watches `fd` on this loop, so waiting for events returns when it gets
  // data, which goes to `received`.
  void Watch(int fd, std::string* const received) {
    ASSERT_EQ(
        loop_->CreateFileEvent(
            fd,
            event_loop::FileEvent::Create(
                [received](event_loop::Loop*, int ready_fd, int) {
                  std::array<char, 4096> buffer{};
                  const ssize_t size =
                      recv(ready_fd, buffer.data(), buffer.size(), 0);
                  if (size > 0) {
                    received->append(buffer.data(), static_cast<size_t>(size));
                  }
                  return event_loop::CallbackStatus::kOk;
                },
                nullptr, event_loop::ToInt(event_loop::EventFlag::kReadable))),
        event_loop::Status::kOk);
  }

  // One iteration of this loop, waiting for events as the server's does.
  void ProcessEvents() { loop_->ProcessEvents(); }

  // Creates a client on `thread` and returns it with the peer's socket.
  std::pair<std::unique_ptr<Client>, int> Connect(IoThread* const thread) {
    std::array<int, 2> fds{};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    EXPECT_EQ(tcp::NonBlock(fds[0]), tcp::TcpStatusCode::kOk);
    auto connection = std::make_unique<connection::Connection>(
        connection::Context{.loop = thread->Loop(), .fd = fds[0]});
    connection->SetState(connection::ConnectionState::kConnected);
    auto client = Client::Create(std::move(connection), &server_);
    thread->Assign(client.get());
    peers_.push_back(fds[1]);
    return {std::move(client), fds[1]};
  }

  // Runs the main loop until `done` or a timeout.
  bool RunUntil(const std::function<bool()>& done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      loop_->Defer([] {});
      loop_->ProcessEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  // Runs the main loop for `duration`.
  void RunFor(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
      loop_->Defer([] {});
      loop_->ProcessEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ~MainThread() {
    for (const int fd : peers_) {
      close(fd);
    }
  }

  // While holding, handed-over clients wait on this thread, as when the main
  // thread is busy, until ReturnHeld runs their commands and returns them
  // together.
  void Hold() { hold_ = true; }
  void ReturnHeld() {
    hold_ = false;
    for (Client* const client : held_) {
      client->RunParsedQueries();
    }
    if (held_thread_ != nullptr) {
      held_thread_->Return(held_);
    }
    held_.clear();
  }
  [[nodiscard]] size_t HeldCount() const { return held_.size(); }

  [[nodiscard]] int HandoffBatches() const { return handoff_batches_; }
  [[nodiscard]] const std::vector<Client*>& Closed() const { return closed_; }

 private:
  std::unique_ptr<event_loop::Loop> loop_;
  std::unique_ptr<db::RedisDb> db_;
  ServerContext server_{.db = db_.get(),
                        .aof = nullptr,
                        .required_password = {},
                        .io_threads = {}};
  std::vector<int> peers_;
  std::vector<Client*> closed_;
  // Every test holds the clients of one thread.
  IoThread* held_thread_{nullptr};
  std::vector<Client*> held_;
  bool hold_{};
  int handoff_batches_{0};
  std::atomic<bool> running_{true};
};

void Send(int fd, std::string_view data) {
  ASSERT_EQ(write(fd, data.data(), data.size()),
            static_cast<ssize_t>(data.size()));
}

// Reads from the peer until `expected` bytes arrive or a timeout.
std::string Receive(MainThread* const main, int fd, size_t expected) {
  std::string received;
  EXPECT_TRUE(main->RunUntil([fd, expected, &received] {
    std::array<char, 4096> buffer{};
    const ssize_t size = recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (size > 0) {
      received.append(buffer.data(), static_cast<size_t>(size));
    }
    return received.size() >= expected;
  }));
  return received;
}
}  // namespace

TEST(IoThreadTest, WakeupSignalsUntilDrained) {
  auto wakeup = Wakeup::Create();
  ASSERT_NE(wakeup, nullptr);
  EXPECT_EQ(event_loop::WaitForEvent(wakeup->ReadFd(),
                                     event_loop::EventFlag::kReadable, 0),
            0);
  wakeup->Signal();
  wakeup->Signal();
  EXPECT_NE(event_loop::WaitForEvent(wakeup->ReadFd(),
                                     event_loop::EventFlag::kReadable, 0),
            0);
  wakeup->Drain();
  EXPECT_EQ(event_loop::WaitForEvent(wakeup->ReadFd(),
                                     event_loop::EventFlag::kReadable, 0),
            0);
}

TEST(IoThreadTest, RunsCommandsOnTheMainThreadAndWritesReplies) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());

  Send(peer, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
  EXPECT_EQ(Receive(&main, peer, 5), "+OK\r\n");
  Send(peer, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  EXPECT_EQ(Receive(&main, peer, 7), "$1\r\nv\r\n");

  const IoThreadInfo info = thread->Info();
  EXPECT_EQ(info.clients, 1);
  EXPECT_GE(info.reads_processed, 2);
  EXPECT_GE(info.writes_processed, 2);
  thread->Stop();
}

TEST(IoThreadTest, NoRoundTripWaitsForATimerWhenWakeupsAreSkipped) {
  MainThread main;
  auto thread = main.StartThreadThatSkipsWakeups();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());
  std::string received;
  main.Watch(peer, &received);
  // Each side sleeps between requests, so each handoff either finds the
  // other awake and sends no wakeup, or finds it asleep and wakes it. A
  // wakeup skipped while the other side was going to sleep would leave the
  // request waiting for a timer: up to a second for the main loop here, and
  // 100 ms for the I/O thread's cron. A wrong condition would delay most
  // round trips; a few slow ones are the machine's scheduling.
  int slow = 0;
  for (int i = 1; i <= 300; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const std::string expected = ":" + std::to_string(i) + "\r\n";
    received.clear();
    Send(peer, "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n");
    while (received.size() < expected.size() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
      main.ProcessEvents();
    }
    ASSERT_EQ(received, expected);
    if (std::chrono::steady_clock::now() - start >=
        std::chrono::milliseconds(90)) {
      ++slow;
    }
  }
  EXPECT_LE(slow, 3);
  thread->Stop();
}

TEST(IoThreadTest, HandsOverAPipelineAtOnceAndRepliesInOrder) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());

  std::string pipeline;
  std::string expected;
  for (int i = 0; i < 20; ++i) {
    pipeline += "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n";
    expected += ":" + std::to_string(i + 1) + "\r\n";
  }
  Send(peer, pipeline);
  EXPECT_EQ(Receive(&main, peer, expected.size()), expected);
  EXPECT_EQ(main.HandoffBatches(), 1);
  thread->Stop();
}

TEST(IoThreadTest, ReturnsSeveralClientsTogetherAndRepliesToEach) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  constexpr size_t kClients = 8;
  std::vector<std::unique_ptr<Client>> clients;
  std::vector<int> peers;
  for (size_t i = 0; i < kClients; ++i) {
    auto [client, peer] = main.Connect(thread.get());
    clients.push_back(std::move(client));
    peers.push_back(peer);
  }

  main.Hold();
  for (const int peer : peers) {
    Send(peer, "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n");
  }
  ASSERT_TRUE(main.RunUntil([&main] { return main.HeldCount() == kClients; }));
  main.ReturnHeld();
  for (size_t i = 0; i < kClients; ++i) {
    EXPECT_EQ(Receive(&main, peers[i], 4),
              ":" + std::to_string(i + 1) + "\r\n");
  }
  // Each client reads again after the batch that returned it.
  Send(peers.back(), "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n");
  EXPECT_EQ(Receive(&main, peers.back(), 4), ":9\r\n");
  thread->Stop();
}

TEST(IoThreadTest, ReturningNoClientsDoesNothing) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());

  thread->Return({});
  Send(peer, "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n");
  EXPECT_EQ(Receive(&main, peer, 4), ":1\r\n");
  thread->Stop();
}

TEST(IoThreadTest, ReadsInputThatArrivedWhileTheMainThreadHeldTheClient) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());

  main.Hold();
  Send(peer, "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n");
  ASSERT_TRUE(main.RunUntil([&main] { return main.HeldCount() == 1; }));
  // The socket turns readable while the client is away: the thread must
  // leave it alone, then read it once the client is back.
  Send(peer, "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n");
  main.RunFor(std::chrono::milliseconds(20));
  main.ReturnHeld();
  EXPECT_EQ(Receive(&main, peer, 8), ":1\r\n:2\r\n");
  thread->Stop();
}

TEST(IoThreadTest, ClosesAClientWhosePeerLeftWhileItWasHeld) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());

  main.Hold();
  Send(peer, "*1\r\n$4\r\nPING\r\n");
  ASSERT_TRUE(main.RunUntil([&main] { return main.HeldCount() == 1; }));
  shutdown(peer, SHUT_WR);
  main.RunFor(std::chrono::milliseconds(20));
  EXPECT_TRUE(main.Closed().empty());
  main.ReturnHeld();
  EXPECT_EQ(Receive(&main, peer, 7), "+PONG\r\n");
  ASSERT_TRUE(main.RunUntil([&main] { return main.Closed().size() == 1; }));
  thread->Stop();
}

TEST(IoThreadTest, HandsOverClosedClients) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [quitting, quitting_peer] = main.Connect(thread.get());
  auto [leaving, leaving_peer] = main.Connect(thread.get());

  // QUIT closes after its reply; a peer that hangs up closes at once.
  Send(quitting_peer, "*1\r\n$4\r\nQUIT\r\n");
  EXPECT_EQ(Receive(&main, quitting_peer, 5), "+OK\r\n");
  shutdown(leaving_peer, SHUT_WR);
  ASSERT_TRUE(main.RunUntil([&main] { return main.Closed().size() == 2; }));
  for (Client* const client : main.Closed()) {
    EXPECT_EQ(client->Connection()->State(),
              connection::ConnectionState::kClosed);
  }
  thread->Stop();
}

TEST(IoThreadTest, PausedThreadHandlesNothingUntilResumed) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());
  // Let the thread start reading the client before pausing it.
  Send(peer, "*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(Receive(&main, peer, 7), "+PONG\r\n");

  thread->Pause();
  Send(peer, "*1\r\n$4\r\nPING\r\n");
  main.RunFor(std::chrono::milliseconds(100));
  EXPECT_EQ(main.HandoffBatches(), 1);
  thread->Resume();
  EXPECT_EQ(Receive(&main, peer, 7), "+PONG\r\n");
  thread->Stop();
}

TEST(IoThreadTest, ReleasesAnIdleClientsBuffersOnItsThread) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());
  Send(peer, "*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(Receive(&main, peer, 7), "+PONG\r\n");

  // Two rounds of the thread's sweeper pass with no request.
  main.RunFor(std::chrono::milliseconds(2500));
  // A paused thread touches no client, so this thread may read one.
  thread->Pause();
  EXPECT_EQ(client->BufferCapacity(), 0);
  thread->Resume();

  Send(peer, "*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(Receive(&main, peer, 7), "+PONG\r\n");
  thread->Stop();
}

TEST(IoThreadTest, StopsWithClientsStillAssigned) {
  MainThread main;
  auto thread = main.StartThread();
  ASSERT_NE(thread, nullptr);
  auto [client, peer] = main.Connect(thread.get());
  Send(peer, "*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(Receive(&main, peer, 7), "+PONG\r\n");
  // The thread stops first; the client then closes its socket alone.
  thread->Stop();
  client.reset();
  thread.reset();
}
}  // namespace redis_simple
