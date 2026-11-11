#ifndef REDIS_SIMPLE_SERVER_RDB_RDB_H_
#define REDIS_SIMPLE_SERVER_RDB_RDB_H_

#include <cstdint>
#include <memory>
#include <string>

namespace redis_simple::aof {
class FileOps;
}

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::rdb {
struct Options {
  // Redis' dbfilename: the RDB file's name in the working directory.
  std::string file_name{"dump.rdb"};
  // Redis' rdbcompression and rdbchecksum.
  bool compression{true};
  bool checksum{true};
  // Tests inject failures here; null uses the real system calls.
  std::shared_ptr<aof::FileOps> file_ops;
};

// The server's RDB snapshots (Redis' rdb.c): loading the file at startup
// and SAVE.
class Rdb {
 public:
  static std::unique_ptr<Rdb> Create(Options options);

  // Loads the file into `db`, as Redis does at startup when the AOF is off.
  // A missing file leaves `db` empty. Returns false after logging why the
  // file could not be loaded, which should stop the server.
  bool Load(db::RedisDb* db) const;

  // SAVE, as Redis' rdbSave does: writes "temp-<pid>.rdb", renames it over
  // the file, and syncs the directory. Returns false after logging a
  // failure, leaving the previous file in place.
  bool Save(db::RedisDb* db);

  // LASTSAVE: when the last successful save finished, in Unix seconds; the
  // server's start time before any.
  int64_t LastSaveTime() const { return last_save_time_; }

  const std::string& FileName() const { return options_.file_name; }

 private:
  explicit Rdb(Options options);

  Options options_;
  int64_t last_save_time_;
};
}  // namespace redis_simple::rdb

#endif  // REDIS_SIMPLE_SERVER_RDB_RDB_H_
