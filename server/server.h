#pragma once

#include <cstdint>
#include <memory>
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
  static Server* Get();
  bool Run(const ServerOptions& options);
  void Stop();
  event_loop::Loop* Loop() { return loop_.get(); }
  db::RedisDb* Db() { return db_.get(); }
  aof::Aof* Aof() { return aof_.get(); }
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
};
}  // namespace redis_simple
