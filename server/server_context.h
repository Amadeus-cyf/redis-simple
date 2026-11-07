#ifndef REDIS_SIMPLE_SERVER_SERVER_CONTEXT_H_
#define REDIS_SIMPLE_SERVER_SERVER_CONTEXT_H_

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
class IoThread;

// The server-wide state commands reach through their client. The server owns
// one and every client points to it, so it must outlive them; tests and AOF
// replay build their own.
struct ServerContext {
  // Never null.
  db::RedisDb* db{nullptr};
  // Null when persistence is off.
  aof::Aof* aof{nullptr};
  // The password clients must AUTH with; empty when AUTH is off.
  std::string_view required_password;
  // Empty when I/O is single-threaded.
  std::span<const std::unique_ptr<IoThread>> io_threads;
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_SERVER_CONTEXT_H_
