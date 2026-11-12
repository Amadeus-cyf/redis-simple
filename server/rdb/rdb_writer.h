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
  // Redis' rdb-save-incremental-fsync: sync every 4 MiB written, instead of
  // only at the end.
  bool incremental_fsync{true};
  // Set the "aof-base" aux field, for an AOF's base file.
  bool aof_base{false};
  // The "ctime" aux field: the creation time in Unix seconds.
  int64_t creation_time{};
};

// The outcome of WriteRdbFile. A background save's child exits with it,
// since it cannot log.
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
