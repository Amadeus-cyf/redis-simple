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
#include "server/io_thread.h"
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
  // Threaded I/O, enabled when --io-threads is above 1, as in Redis 8: each
  // client belongs to one I/O thread, which reads and frames its requests and
  // writes its replies, while commands run here on the main thread. Returns
  // the thread a new client should use (the one with the fewest clients), or
  // null when I/O is single-threaded.
  IoThread* NextIoThread();
  const std::vector<std::unique_ptr<IoThread>>& IoThreads() const {
    return io_threads_;
  }
  // Pauses every I/O thread around work such as fork() that must not race
  // their allocations.
  void PauseIoThreads();
  void ResumeIoThreads();
  ~Server() = default;

 private:
  Server();
  bool InstallAcceptCallback();
  int64_t ServerCron();
  bool StartIoThreads(size_t thread_count);
  void StopIoThreads();
  // Runs the commands of the clients an I/O thread handed over, flushes the
  // AOF once for all of them, and hands them back to write the replies.
  void RunHandedOverClients(IoThread* thread,
                            std::vector<IoThread::Handoff>* handoffs);
  int fd_{-1};
  std::unique_ptr<event_loop::Loop> loop_;
  std::unordered_map<Client*, std::unique_ptr<Client>> clients_;
  std::unique_ptr<db::RedisDb> db_;
  std::unique_ptr<aof::Aof> aof_;
  std::string require_pass_;
  bool protected_mode_{true};
  std::vector<std::unique_ptr<IoThread>> io_threads_;
  // Clients of the handoff being run, reused between handoffs.
  std::vector<Client*> handed_over_;
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_SERVER_H_
