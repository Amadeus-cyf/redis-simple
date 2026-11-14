#ifndef REDIS_SIMPLE_SERVER_DB_DB_H_
#define REDIS_SIMPLE_SERVER_DB_DB_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "containers/dict.h"
#include "server/db/async_reclaimer.h"
#include "server/db/key_watcher.h"
#include "server/db/redis_object.h"

namespace redis_simple::aof {
class Aof;
}
namespace redis_simple::db {
class RedisDb;
}
namespace redis_simple::rdb {
struct LoadOptions;
struct LoadStats;
bool LoadRdb(int fd, db::RedisDb* db, const LoadOptions& options,
             LoadStats* stats);
}  // namespace redis_simple::rdb

namespace redis_simple::db {
enum class DbStatus {
  kOk = 0,
  kError = -1,
};

// What SetKey does with an existing TTL when no new expiration is given.
enum class ExistingTtl {
  kClear,
  kKeep,
};

enum class TtlResolution {
  kSeconds,
  kMilliseconds,
};

enum class ExpireCondition {
  kNone,
  kIfNoExpiry,
  kIfHasExpiry,
  kGreaterThan,
  kLessThan,
  // XX and LT together: the key must have a TTL, and the new one is earlier.
  kIfHasExpiryAndLessThan,
};

struct ExpireSampleResult {
  size_t sampled{};
  size_t expired{};
};

class RedisDb {
 public:
  static std::unique_ptr<RedisDb> Create();
  const RedisObject* LookupKey(std::string_view key);
  RedisObject* MutableLookupKey(std::string_view key);
  // `expire` is an absolute time in milliseconds, or 0 for none.
  DbStatus SetKey(std::string_view key, RedisObjectPtr object, int64_t expire,
                  ExistingTtl existing_ttl = ExistingTtl::kClear);
  // Adds a key that should not exist yet, hashing it once, as loading a
  // file does (Redis' dbAddRDBLoad). Returns false, changing nothing, if the
  // key exists. `expire` is as for SetKey.
  bool AddKey(std::string_view key, RedisObjectPtr object, int64_t expire);
  // Like SetKey with a string value, but overwrites an existing string
  // object in place instead of allocating a new one.
  DbStatus SetStringKey(std::string_view key, std::string_view value,
                        int64_t expire,
                        ExistingTtl existing_ttl = ExistingTtl::kClear);
  DbStatus DeleteKey(std::string_view key);
  DbStatus UnlinkKey(std::string_view key);
  DbStatus ExpireKeyAt(std::string_view key, int64_t expire,
                       ExpireCondition condition = ExpireCondition::kNone);
  DbStatus PersistKey(std::string_view key);
  DbStatus RenameKey(std::string_view old_key, std::string_view new_key);
  std::optional<int64_t> Expiration(std::string_view key) const;
  // A live key's object and TTL, hashing the key once for both lookups.
  struct KeyWithExpiration {
    const RedisObject* object{nullptr};
    std::optional<int64_t> expire;
  };
  KeyWithExpiration LookupKeyWithExpiration(std::string_view key);
  int64_t TimeToLive(std::string_view key, TtlResolution resolution);
  size_t KeyCount() const { return dict_->Size(); }
  size_t KeysWithExpirationCount() const { return expires_->Size(); }
  // Key views and object references remain valid only until the database is
  // mutated. Returning false from the visitor stops further callbacks.
  template <typename Visitor>
  bool ForEachObject(Visitor&& visitor);
  // One SCAN step (containers::Dict::ScanStep): calls `visitor(key, object)`
  // for each live key it visits from `cursor`, and returns the next cursor,
  // 0 when done. Expired keys count toward `count` but are not visited, as
  // in Redis. Key views remain valid until the database is mutated.
  template <typename Visitor>
  size_t ScanKeys(size_t cursor, size_t count, Visitor&& visitor);
  void Flush();
  ExpireSampleResult ExpireSome(size_t max_samples, int64_t now);
  // Pins the clock while one command (or one EXEC) runs, so expiration
  // checks and relative TTLs all see a single time, as Redis'
  // commandTimeSnapshot does. The clock is read on first use, so a command
  // that needs no time does not read it.
  void PinCommandTime() { time_pinned_ = true; }
  void UnpinCommandTime() {
    time_pinned_ = false;
    command_time_.reset();
  }
  // Pins a given time, for tests.
  void SetCommandTime(int64_t now_ms) {
    time_pinned_ = true;
    command_time_ = now_ms;
  }
  // The time in milliseconds: the pinned time while a command runs.
  int64_t Now() const {
    return command_time_.has_value() ? *command_time_ : ReadClock();
  }
  // WATCH support. Key-level mutations made through this class (set, delete,
  // expire, persist, rename, flush, and expiration itself) mark every watcher
  // of the key dirty. Commands that edit a value in place call
  // TouchWatchedKey after a successful mutation.
  void WatchKey(std::string_view key, KeyWatcher* watcher);
  void UnwatchKeys(KeyWatcher* watcher);
  void TouchWatchedKey(std::string_view key);
  bool HasWatchedKeys() const { return watched_keys_->Size() > 0; }
  // Holds the thread that frees UNLINKed values idle, for fork().
  void PauseBackgroundFrees() { reclaimer_.Pause(); }
  void ResumeBackgroundFrees() { reclaimer_.Resume(); }

 private:
  friend class aof::Aof;
  friend bool rdb::LoadRdb(int fd, RedisDb* db, const rdb::LoadOptions& options,
                           rdb::LoadStats* stats);
  RedisDb();
  void SetLoading(bool loading) { loading_ = loading; }
  // Each public operation hashes its key once and passes the HashedKey to
  // the keyspace, TTL, and WATCH tables, which share one hash seed.
  RedisObject* Lookup(const containers::HashedKey& key);
  // TTL and WATCH bookkeeping shared by SetKey and SetStringKey once the
  // value is stored.
  void FinishSet(const containers::HashedKey& key, int64_t expire,
                 ExistingTtl existing_ttl);
  DbStatus Delete(const containers::HashedKey& key);
  void TouchWatchedKey(const containers::HashedKey& key);
  bool IsKeyExpired(std::string_view key) const;
  bool IsKeyExpired(const containers::HashedKey& key) const;
  // Reads the clock, and keeps the time if it is pinned.
  int64_t ReadClock() const;
  std::unique_ptr<containers::Dict<std::string, RedisObjectPtr>> dict_;
  std::unique_ptr<containers::Dict<std::string, int64_t>> expires_;
  std::unique_ptr<containers::Dict<std::string, std::vector<KeyWatcher*>>>
      watched_keys_;
  AsyncReclaimer reclaimer_;
  size_t expire_cursor_{};
  // Set on first use while the time is pinned.
  mutable std::optional<int64_t> command_time_;
  bool time_pinned_{};
  // Replay defers expiration checks until all historical writes are applied.
  bool loading_{};
};

template <typename Visitor>
bool RedisDb::ForEachObject(Visitor&& visitor) {
  if (dict_->Size() == 0) {
    return true;
  }

  bool keep_visiting = true;
  std::optional<size_t> cursor = 0;
  while (cursor.has_value()) {
    cursor = dict_->Scan(
        *cursor, [this, &visitor, &keep_visiting](
                     const std::string& key, const RedisObjectPtr& object) {
          if (keep_visiting && !IsKeyExpired(key)) {
            keep_visiting = visitor(std::string_view(key), *object);
          }
        });
    if (!keep_visiting) {
      return false;
    }
  }
  return true;
}

template <typename Visitor>
size_t RedisDb::ScanKeys(size_t cursor, size_t count, Visitor&& visitor) {
  if (count == 0) {
    return 0;
  }
  return dict_->ScanStep(cursor, count,
                         // Take each key with its stored hash, so the TTL
                         // lookup does not hash it again.
                         [this, &visitor](const containers::HashedKey& key,
                                          const RedisObjectPtr& object) {
                           if (!IsKeyExpired(key)) {
                             visitor(key.key, *object);
                           }
                         });
}
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_DB_H_
