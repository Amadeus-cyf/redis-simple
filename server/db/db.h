#ifndef REDIS_SIMPLE_SERVER_DB_DB_H_
#define REDIS_SIMPLE_SERVER_DB_DB_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "containers/dict.h"
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
  // A key located for a write, defined below.
  class KeySlot;
  // Looks `key` up for a command that goes on to write it. The slot's
  // object is null when the key is missing or has expired.
  KeySlot FindKeyForWrite(std::string_view key);
  // FindKeyForWrite that adds a missing key, hashing the key and probing the
  // keyspace once for both, as Redis' lookupKeyWriteWithLink and dbAddByLink
  // look a key up and then add it at the place the lookup found. A created
  // key holds an empty string and has no TTL; the caller stores its value
  // before returning.
  KeySlot FindOrCreateKey(std::string_view key);
  // SetStringKey for a key already located by `slot`, which must hold an
  // object.
  void SetStringAt(const KeySlot& slot, std::string_view value, int64_t expire,
                   ExistingTtl existing_ttl = ExistingTtl::kClear);
  DbStatus DeleteKey(std::string_view key);
  // DeleteKey, but a value of more than 64 elements is freed in steps by
  // FreeLazily, so other clients are not held up.
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
  // Empties the database. A large keyspace is detached and freed in steps by
  // FreeLazily, so the flush does not hold up other clients.
  void Flush();
  // Frees what UNLINK and Flush detached, for about `budget_milliseconds`;
  // the server cron calls it every tick. Returns whether any remains.
  bool FreeLazily(int64_t budget_milliseconds);
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

 private:
  // Objects live inside their keyspace entries, after the key's header and
  // before its name, as Redis 8 keeps a key and its value in one block
  // (kvobj): one allocation per key, and no pointer from entry to object.
  using Keyspace = containers::Dict<std::string, RedisObject>;
  using KeyRef = Keyspace::EntryRef;
  // The TTL table's keys refer to keyspace entries rather than copying their
  // names. Its hash is the keyspace's stored hash, so a HashedKey finds both.
  struct KeyRefHash {
    size_t operator()(const KeyRef& key) const { return key.StoredHash(); }
    size_t operator()(const containers::HashedKey& key) const {
      return key.hash;
    }
    size_t operator()(std::string_view key) const {
      return containers::DictHash<std::string>{}(key);
    }
  };
  struct KeyRefEqual {
    bool operator()(const KeyRef& left, const KeyRef& right) const {
      return left == right;
    }
    bool operator()(const containers::HashedKey& left,
                    const KeyRef& right) const {
      return left.key == right.Key();
    }
    bool operator()(std::string_view left, const KeyRef& right) const {
      return left == right.Key();
    }
  };
  // Records `expire` for the key `entry` refers to.
  void SetExpire(KeyRef entry, int64_t expire);
  // Deletes a key whose view points into its own keyspace entry, as keys
  // found by scanning the TTL table do.
  void DeleteStoredKey(std::string_view key);

  friend class aof::Aof;
  friend bool rdb::LoadRdb(int fd, RedisDb* db, const rdb::LoadOptions& options,
                           rdb::LoadStats* stats);
  RedisDb();
  void SetLoading(bool loading) { loading_ = loading; }
  // Each public operation hashes its key once and passes the HashedKey to
  // the keyspace, TTL, and WATCH tables, which share one hash seed.
  RedisObject* Lookup(const containers::HashedKey& key);
  // Finds `key`'s keyspace entry, adding it if missing, without checking
  // whether an existing key has expired.
  KeySlot FindOrInsert(const containers::HashedKey& key);
  // TTL and WATCH bookkeeping shared by the setters once the value is
  // stored in `slot`.
  void FinishSet(const KeySlot& slot, int64_t expire, ExistingTtl existing_ttl);
  DbStatus Delete(const containers::HashedKey& key);
  void TouchWatchedKey(const containers::HashedKey& key);
  bool IsKeyExpired(std::string_view key) const;
  bool IsKeyExpired(const containers::HashedKey& key) const;
  // Reads the clock, and keeps the time if it is pinned.
  int64_t ReadClock() const;
  using ExpireTable =
      containers::Dict<KeyRef, int64_t, KeyRefHash, KeyRefEqual>;
  // Tables a flush detached, waiting to be freed. The TTL table is freed
  // first, as for a single key.
  struct FlushedTables {
    std::unique_ptr<ExpireTable> expires;
    std::unique_ptr<Keyspace> keyspace;
  };

  std::unique_ptr<Keyspace> dict_;
  // Every TTL entry reads its key from a keyspace entry, so a key's TTL
  // must be removed before its keyspace entry is deleted.
  std::unique_ptr<ExpireTable> expires_;
  std::deque<FlushedTables> flushed_;
  // Large values UNLINK detached, waiting to be freed.
  std::deque<RedisObject> unlinked_;
  std::unique_ptr<containers::Dict<std::string, std::vector<KeyWatcher*>>>
      watched_keys_;
  size_t expire_cursor_{};
  // Set on first use while the time is pinned.
  mutable std::optional<int64_t> command_time_;
  bool time_pinned_{};
  // Replay defers expiration checks until all historical writes are applied.
  bool loading_{};
};

// Carries a key's hash and keyspace entry from the lookup to the write that
// follows, so the write neither hashes the key nor probes the keyspace
// again, as Redis passes a dictEntryLink from lookupKeyWriteWithLink to
// setKeyByLink. Valid until the database is next changed through another
// key, and no longer than the key's bytes.
class RedisDb::KeySlot {
 public:
  RedisObject* Object() const { return object_; }
  // Whether FindOrCreateKey added the key.
  bool Created() const { return created_; }

 private:
  friend class RedisDb;
  KeySlot(const containers::HashedKey& key, RedisObject* object, KeyRef entry,
          bool created)
      : key_(key), object_(object), entry_(entry), created_(created) {}

  containers::HashedKey key_;
  RedisObject* object_;
  // Empty when FindKeyForWrite located the key: Dict's value lookup does not
  // return the entry.
  KeyRef entry_;
  bool created_;
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
        *cursor, [this, &visitor, &keep_visiting](std::string_view key,
                                                  const RedisObject& object) {
          if (keep_visiting && !IsKeyExpired(key)) {
            keep_visiting = visitor(std::string_view(key), object);
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
                                          const RedisObject& object) {
                           if (!IsKeyExpired(key)) {
                             visitor(key.key, object);
                           }
                         });
}
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_DB_H_
