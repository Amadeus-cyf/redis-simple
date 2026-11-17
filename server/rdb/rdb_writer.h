#ifndef REDIS_SIMPLE_SERVER_RDB_RDB_WRITER_H_
#define REDIS_SIMPLE_SERVER_RDB_RDB_WRITER_H_

#include <cstdint>
#include <string>

namespace redis_simple::aof {
class FileOps;
}

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::rdb {
struct WriteOptions {
  // Redis' rdbcompression: LZF-compress strings longer than 20 bytes when
  // that saves at least four bytes.
  bool compression{true};
  // Redis' rdbchecksum: end the file with a CRC64 of its contents, or zero.
  bool checksum{true};
  // Redis' rdb-save-incremental-fsync: push every 4 MiB written towards the
  // disk (aof::IncrementalSync), instead of syncing only at the end.
  bool incremental_fsync{true};
  // Redis' RDBFLAGS_KEEP_CACHE: leave the written file in the kernel's page
  // cache instead of dropping it, for a file that is about to be read.
  bool keep_page_cache{false};
  // Set the "aof-base" aux field, for an AOF's base file.
  bool aof_base{false};
  // The "ctime" aux field: the creation time in Unix seconds.
  int64_t creation_time{};
};

// The outcome of WriteRdbFile. The forked children that call it cannot log,
// so they report it through their exit status.
enum class WriteStatus : uint8_t {
  kWritten = 0,
  kWriteFailed = 1,
  kSyncFailed = 2,
};

// Writes the database to `path` in Redis' RDB format, as Redis' rdbSaveRio
// does: the header and aux fields, the keys with their expiration times,
// the EOF opcode, and the checksum. Values are stored in the encodings Redis
// stores them in, so Redis loads the file, and listpacks and intsets are
// written as they are. Syncs the file and deletes it on failure.
//
// It takes no locks and does not log, so a forked child can run it.
WriteStatus WriteRdbFile(db::RedisDb* db, const std::string& path,
                         const WriteOptions& options, aof::FileOps* file_ops);
}  // namespace redis_simple::rdb

#endif  // REDIS_SIMPLE_SERVER_RDB_RDB_WRITER_H_
