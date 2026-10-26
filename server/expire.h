#pragma once

namespace redis_simple {
namespace db {
class RedisDb;
}  // namespace db

// Deletes a bounded sample of expired keys from `db`.
void ActiveExpireCycle(db::RedisDb* db);
}  // namespace redis_simple
