#ifndef REDIS_SIMPLE_SERVER_EXPIRE_H_
#define REDIS_SIMPLE_SERVER_EXPIRE_H_

namespace redis_simple {
namespace db {
class RedisDb;
}  // namespace db

// Deletes a bounded sample of expired keys from `db`.
void ActiveExpireCycle(db::RedisDb* db);
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_EXPIRE_H_
