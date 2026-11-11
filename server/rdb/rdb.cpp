#include "server/rdb/rdb.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "logging/logger.h"
#include "server/aof/file_ops.h"
#include "server/db/db.h"
#include "server/rdb/rdb_loader.h"
#include "server/rdb/rdb_writer.h"
#include "utils/time_utils.h"

namespace redis_simple::rdb {
namespace {
int64_t UnixSeconds() { return utils::NowInMilliseconds() / 1000; }
}  // namespace

Rdb::Rdb(Options options)
    : options_(std::move(options)), last_save_time_(UnixSeconds()) {
  if (options_.file_ops == nullptr) {
    options_.file_ops = aof::DefaultFileOps();
  }
}

std::unique_ptr<Rdb> Rdb::Create(Options options) {
  return std::unique_ptr<Rdb>(new Rdb(std::move(options)));
}

bool Rdb::Load(db::RedisDb* const db) const {
  const int64_t start = utils::NowInMilliseconds();
  const int fd = aof::OpenFile(options_.file_name, O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      return true;
    }
    RS_LOG_ERROR("Fatal error opening the RDB file %s: %s\n",
                 options_.file_name.c_str(), std::strerror(errno));
    return false;
  }
  LoadStats stats;
  const bool loaded =
      LoadRdb(fd, db, {.now_ms = utils::NowInMilliseconds()}, &stats);
  close(fd);
  if (!loaded) {
    RS_LOG_ERROR("Fatal error loading the DB, check server logs. Exiting.\n");
    return false;
  }
  RS_LOG_INFO(
      "Done loading RDB, keys loaded: %zu, keys expired: %zu, empty keys "
      "skipped: %zu.\n",
      stats.keys_loaded, stats.keys_expired, stats.empty_keys_skipped);
  RS_LOG_INFO("DB loaded from disk: %.3f seconds\n",
              static_cast<double>(utils::NowInMilliseconds() - start) / 1000);
  return true;
}

bool Rdb::Save(db::RedisDb* const db) {
  const std::string temporary = "temp-" + std::to_string(getpid()) + ".rdb";
  const WriteStatus status = WriteRdbFile(db, temporary,
                                          {.compression = options_.compression,
                                           .checksum = options_.checksum,
                                           .creation_time = UnixSeconds()},
                                          options_.file_ops.get());
  if (status != WriteStatus::kWritten) {
    RS_LOG_WARN(
        "Write error saving DB on disk: %s\n",
        status == WriteStatus::kSyncFailed ? "fsync failed" : "write failed");
    return false;
  }
  if (options_.file_ops->Rename(temporary.c_str(), options_.file_name.c_str()) <
      0) {
    RS_LOG_WARN(
        "Error moving temp DB file %s on the final destination %s: %s\n",
        temporary.c_str(), options_.file_name.c_str(), std::strerror(errno));
    unlink(temporary.c_str());
    return false;
  }
  if (!aof::SyncDirectory(options_.file_ops.get(), ".")) {
    RS_LOG_WARN("Failed to fsync directory while saving DB: %s\n",
                std::strerror(errno));
    return false;
  }
  RS_LOG_INFO("DB saved on disk\n");
  last_save_time_ = UnixSeconds();
  return true;
}
}  // namespace redis_simple::rdb
