#ifndef REDIS_SIMPLE_SERVER_SERVER_H_
#define REDIS_SIMPLE_SERVER_SERVER_H_

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "event_loop/loop.h"
#include "server/aof.h"
#include "server/client.h"
#include "server/io_threads.h"
#include "server/server_options.h"

namespace redis_simple {
class Server {
 public:
  static std::unique_ptr<Server> Create();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  bool Run(const ServerOptions& options);
  void Stop();
  event_loop::Loop* Loop() { return loop_.get(); }
  db::RedisDb* Db() { return db_.get(); }
  aof::Aof* Aof() { return aof_.get(); }
  // Password every new client must AUTH with; empty when auth is disabled.
  std::string_view RequirePass() const { return require_pass_; }
  bool ProtectedMode() const { return protected_mode_; }
  void AddClient(std::unique_ptr<Client> client) {
    Client* const key = client.get();
    clients_.emplace(key, std::move(client));
  }
  bool RemoveClient(Client* c);
  // Threaded I/O, enabled when --io-threads is above 1. Readable clients are
  // queued, and HandlePendingIo runs before the loop waits again: it reads
  // every queued client on the I/O threads, runs their commands in order on
  // this thread, flushes the AOF once, then writes the replies on the I/O
  // threads. Commands and data structures stay single-threaded.
  bool ThreadedIo() const { return io_threads_ != nullptr; }
  const IoStats* ThreadedIoStats() const { return &io_stats_; }
  void QueueRead(Client* client);
  void QueueWrite(Client* client);
  ~Server() = default;

 private:
  Server();
  bool InstallAcceptCallback();
  int64_t ServerCron();
  void HandlePendingIo();
  void ReadPendingClients();
  void WritePendingClients();
  int fd_{-1};
  std::unique_ptr<event_loop::Loop> loop_;
  std::unordered_map<Client*, std::unique_ptr<Client>> clients_;
  std::unique_ptr<db::RedisDb> db_;
  std::unique_ptr<aof::Aof> aof_;
  std::string require_pass_;
  bool protected_mode_{true};
  std::unique_ptr<IoThreads> io_threads_;
  IoStats io_stats_;
  // Clients waiting for the next I/O batch, and the batch being handled.
  std::vector<Client*> pending_reads_;
  std::vector<Client*> pending_writes_;
  std::vector<Client*> io_batch_;
  // Each batch client's read or write result, filled in by the I/O threads.
  std::vector<ssize_t> io_results_;
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_SERVER_H_
