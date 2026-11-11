#ifndef REDIS_SIMPLE_SERVER_RDB_RDB_LOADER_H_
#define REDIS_SIMPLE_SERVER_RDB_RDB_LOADER_H_

#include <cstddef>
#include <cstdint>

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::rdb {
struct LoadOptions {
  // The time keys expire against, in Unix milliseconds.
  int64_t now_ms{};
  // Keep keys whose expiration time has passed, as Redis does for an AOF's
  // base file, whose later records may still refer to them. Otherwise they
  // are skipped, as Redis does when a primary loads an RDB file.
  bool keep_expired{false};
};

struct LoadStats {
  size_t keys_loaded{};
  size_t keys_expired{};
  size_t empty_keys_skipped{};
  // The bytes the RDB data took, from its header through its checksum.
  size_t bytes{};
};

// Loads the RDB data that starts at `fd`'s current position into `db`, as
// Redis' rdbLoadRio does, and returns false after logging why it could not.
// Reads strings, lists, sets, sorted sets, and hashes in every encoding
// Redis 7.0 and later write (and the plain encodings of earlier versions),
// with their expiration times; refuses streams, modules, hash field
// expiration, hash templates, ziplists, and databases other than 0, which
// this server does not have. Values are rebuilt through the data types, so
// they take this server's encodings. Corrupt data is reported, never
// trusted: lengths are checked against the file, encoded listpacks and
// intsets are validated, and duplicates are rejected.
//
// `db` may hold keys loaded before; a key the data repeats is an error.
bool LoadRdb(int fd, db::RedisDb* db, const LoadOptions& options,
             LoadStats* stats);
}  // namespace redis_simple::rdb

#endif  // REDIS_SIMPLE_SERVER_RDB_RDB_LOADER_H_
