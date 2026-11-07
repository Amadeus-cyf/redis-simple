#ifndef REDIS_SIMPLE_SERVER_AOF_SNAPSHOT_H_
#define REDIS_SIMPLE_SERVER_AOF_SNAPSHOT_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::aof {
class FileOps;

// Bounds snapshot memory and the size of each replayed command together, so
// a snapshot always loads within the replay limit.
struct Limits {
  // Replay rejects a command larger than this.
  size_t max_replay_command_bytes{size_t{64} * 1024 * 1024};
  // The rewrite writes the snapshot in blocks of about this size, and splits
  // large values and collections into commands no larger.
  size_t snapshot_block_bytes{size_t{1024} * 1024};
};

struct SnapshotOptions {
  Limits limits;
  // Start the file with a "#TS:<unix time>" annotation.
  bool timestamp{false};
  // Sync every 4 MiB written, instead of only at the end (Redis'
  // aof-rewrite-incremental-fsync), so the final sync does not stall.
  bool incremental_fsync{true};
};

// The outcome of WriteSnapshotFile, also the rewrite child's exit code, since
// the child cannot log.
enum class SnapshotStatus : uint8_t {
  kWritten = 0,
  kWriteFailed = 1,
  kSyncFailed = 2,
  kEncodeFailed = 3,
};

// Writes the database to `path` as commands that rebuild it (Redis'
// rewriteAppendOnlyFile): SET, or SET plus APPENDs for a large string, and
// batched SADD, RPUSH, ZADD, and HSET, each followed by PEXPIREAT for a key
// with a TTL. Syncs the file and deletes it on failure.
//
// It runs in the rewrite's forked child, so it takes no locks and does not
// log; allocating is safe because the parent forks only while no other
// thread allocates.
SnapshotStatus WriteSnapshotFile(db::RedisDb* db, const std::string& path,
                                 const SnapshotOptions& options,
                                 FileOps* file_ops);
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_SNAPSHOT_H_
