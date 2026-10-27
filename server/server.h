#ifndef REDIS_SIMPLE_SERVER_SERVER_H_
#define REDIS_SIMPLE_SERVER_SERVER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "connection/connection.h"
#include "event_loop/loop.h"
#include "server/aof.h"
#include "server/client.h"
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
  ~Server() = default;

 private:
  Server();
  bool InstallAcceptCallback();
  int64_t ServerCron();
  int fd_{-1};
  std::unique_ptr<event_loop::Loop> loop_;
  std::unordered_map<Client*, std::unique_ptr<Client>> clients_;
  std::unique_ptr<db::RedisDb> db_;
  std::unique_ptr<aof::Aof> aof_;
  std::string require_pass_;
  bool protected_mode_{true};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_SERVER_H_
