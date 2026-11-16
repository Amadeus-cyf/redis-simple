#ifndef REDIS_SIMPLE_SERVER_SERVER_CONTEXT_H_
#define REDIS_SIMPLE_SERVER_SERVER_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace redis_simple {
namespace aof {
class Aof;
}
namespace db {
class RedisDb;
}
namespace rdb {
class Rdb;
}
class IoThread;

// The server-wide state commands reach through their client. The server owns
// one and every client points to it, so it must outlive them; tests and AOF
// replay build their own.
// Redis' fork statistics (INFO stats).
struct ForkStats {
  // How long the last successful fork took (Redis' stat_fork_time).
  int64_t latest_fork_usec{};
  int64_t total_forks{};
};

struct ServerContext {
  // Never null.
  db::RedisDb* db{nullptr};
  // Null when persistence is off.
  aof::Aof* aof{nullptr};
  // The RDB file's snapshots. Null only for contexts tests and AOF replay
  // build.
  rdb::Rdb* rdb{nullptr};
  // Redis' server.dirty, the changes since the last save, which `rdb` owns
  // (Rdb::ChangeCounter). Null where `rdb` is.
  int64_t* changes{nullptr};
  // The password clients must AUTH with; empty when AUTH is off.
  std::string_view required_password;
  // Empty when I/O is single-threaded.
  std::span<const std::unique_ptr<IoThread>> io_threads;
  // Null only for contexts tests and AOF replay build.
  const ForkStats* forks{nullptr};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_SERVER_CONTEXT_H_
