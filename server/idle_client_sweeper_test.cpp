#include "server/idle_client_sweeper.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "server/client.h"
#include "server/db/db.h"
#include "tcp/tcp.h"

namespace redis_simple {
namespace {
constexpr std::string_view kPing = "*1\r\n$4\r\nPING\r\n";

// Clients on socket pairs, each with buffers a request has allocated.
class SweptClients {
 public:
  SweptClients() = default;
  SweptClients(const SweptClients&) = delete;
  SweptClients& operator=(const SweptClients&) = delete;
  SweptClients(SweptClients&&) = delete;
  SweptClients& operator=(SweptClients&&) = delete;
  ~SweptClients() {
    for (const int fd : peers_) {
      close(fd);
    }
  }

  IdleClientSweeper* Sweeper() { return &sweeper_; }
  [[nodiscard]] Client* At(size_t index) const {
    return clients_[index].get();
  }

  void AddActiveClients(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      AddActiveClient();
    }
  }

  Client* AddActiveClient() {
    std::array<int, 2> fds{};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    EXPECT_EQ(tcp::NonBlock(fds[0]), tcp::TcpStatusCode::kOk);
    auto connection = std::make_unique<connection::Connection>(
        connection::Context{.loop = nullptr, .fd = fds[0]});
    connection->SetState(connection::ConnectionState::kConnected);
    clients_.push_back(Client::Create(std::move(connection), &server_));
    peers_.push_back(fds[1]);
    Client* const client = clients_.back().get();
    Request(clients_.size() - 1);
    sweeper_.Add(client);
    return client;
  }

  // Sends one PING to the client at `index` and has it answer.
  void Request(size_t index) {
    Client* const client = clients_[index].get();
    const int peer = peers_[index];
    EXPECT_EQ(write(peer, kPing.data(), kPing.size()),
              static_cast<ssize_t>(kPing.size()));
    EXPECT_EQ(client->ReadQuery(), static_cast<ssize_t>(kPing.size()));
    EXPECT_EQ(client->ProcessInputBuffer(), ClientStatus::kOk);
    while (client->HasPendingReplies()) {
      EXPECT_GT(client->SendReply(), 0);
    }
    EXPECT_GT(client->BufferCapacity(), 0);
  }

  // One round visits every client once.
  void SweepRound() {
    for (size_t i = 0; i < IdleClientSweeper::kSweepsPerRound; ++i) {
      sweeper_.Sweep();
    }
  }

  [[nodiscard]] size_t ReleasedCount() const {
    size_t released = 0;
    for (const std::unique_ptr<Client>& client : clients_) {
      released += client->BufferCapacity() == 0 ? 1 : 0;
    }
    return released;
  }

 private:
  std::unique_ptr<db::RedisDb> db_{db::RedisDb::Create()};
  ServerContext server_{.db = db_.get(),
                        .aof = nullptr,
                        .required_password = {},
                        .io_threads = {}};
  std::vector<std::unique_ptr<Client>> clients_;
  std::vector<int> peers_;
  IdleClientSweeper sweeper_;
};

TEST(IdleClientSweeperTest, ReleasesClientsIdleForAWholeRound) {
  SweptClients clients;
  clients.AddActiveClients(25);
  // The first round sees that every client has read since it was created.
  clients.SweepRound();
  EXPECT_EQ(clients.ReleasedCount(), 0);

  clients.Request(7);
  clients.SweepRound();
  EXPECT_EQ(clients.ReleasedCount(), 24);
  EXPECT_GT(clients.At(7)->BufferCapacity(), 0);

  clients.SweepRound();
  EXPECT_EQ(clients.ReleasedCount(), 25);
}

TEST(IdleClientSweeperTest, OneSweepVisitsAShareOfTheClients) {
  SweptClients clients;
  clients.AddActiveClients(40);
  clients.SweepRound();
  for (size_t sweep = 1; sweep <= IdleClientSweeper::kSweepsPerRound; ++sweep) {
    clients.Sweeper()->Sweep();
    EXPECT_EQ(clients.ReleasedCount(), sweep * 4);
  }
}

TEST(IdleClientSweeperTest, SkipsClientsHandedToTheMainThread) {
  SweptClients clients;
  Client* const client = clients.AddActiveClient();
  client->SetHandedToMain(true);
  clients.SweepRound();
  clients.SweepRound();
  EXPECT_GT(client->BufferCapacity(), 0);

  client->SetHandedToMain(false);
  clients.SweepRound();
  clients.SweepRound();
  EXPECT_EQ(client->BufferCapacity(), 0);
}

TEST(IdleClientSweeperTest, RemovedClientsAreNotVisited) {
  SweptClients clients;
  clients.AddActiveClients(12);
  IdleClientSweeper* const sweeper = clients.Sweeper();
  // Removing mid-round moves the last client into the freed place.
  sweeper->Sweep();
  sweeper->Remove(clients.At(0));
  sweeper->Remove(clients.At(5));
  sweeper->Remove(clients.At(5));
  EXPECT_EQ(sweeper->ClientCount(), 10);
  clients.SweepRound();
  clients.SweepRound();
  clients.SweepRound();
  EXPECT_EQ(clients.ReleasedCount(), 10);
  EXPECT_GT(clients.At(0)->BufferCapacity(), 0);
  EXPECT_GT(clients.At(5)->BufferCapacity(), 0);

  sweeper->Clear();
  EXPECT_EQ(sweeper->ClientCount(), 0);
  sweeper->Sweep();
}
}  // namespace
}  // namespace redis_simple
