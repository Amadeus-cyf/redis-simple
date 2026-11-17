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
#include "containers/handle_set.h"
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
// kError means nothing was changed: the key is missing, or the operation's
// condition was not met.
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

// One database: the keyspace with each key's value and expiration time, the
// index of keys that have a TTL, and the WATCH registrations. Used from the
// main thread only. Lookups delete a key they find expired.
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
  // Sets the key's expiration to `expire`, an absolute time in milliseconds,
  // if `condition` holds. A time that has already passed deletes the key,
  // except while loading.
  DbStatus ExpireKeyAt(std::string_view key, int64_t expire,
                       ExpireCondition condition = ExpireCondition::kNone);
  DbStatus PersistKey(std::string_view key);
  DbStatus RenameKey(std::string_view old_key, std::string_view new_key);
  // The key's expiration time, whether or not it has passed.
  std::optional<int64_t> Expiration(std::string_view key);
  // A live key's object and TTL, both read from its keyspace entry.
  struct KeyWithExpiration {
    const RedisObject* object{nullptr};
    std::optional<int64_t> expire;
  };
  KeyWithExpiration LookupKeyWithExpiration(std::string_view key);
  // The time the key has left, as TTL and PTTL reply: -2 if the key is
  // missing, -1 if it has no expiration. Seconds are rounded to the nearest.
  int64_t TimeToLive(std::string_view key, TtlResolution resolution);
  // Sizes the keyspace for `keys` more keys, `keys_with_expiration` of them
  // with a TTL, before they are added (Redis' dbExpand), so that adding them
  // does not grow and rehash the tables again and again.
  void ReserveKeys(size_t keys, size_t keys_with_expiration);
  size_t KeyCount() const { return dict_->Size(); }
  size_t KeysWithExpirationCount() const { return expires_.Size(); }
  // Calls `visitor(key, object, expire)` for each live key; `expire` is a
  // std::optional<int64_t>. Key views and object references remain valid
  // only until the database is mutated. Returning false from the visitor
  // stops further callbacks.
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
  // Hash table upkeep that otherwise only commands drive, for about
  // `budget_microseconds`; the server cron calls it every tick while no
  // child runs, as Redis' databasesCron does. Moves entries of a keyspace
  // or TTL index rehash under way, so an idle server does not keep both the
  // old and the new table, and starts a shrink that was held back while a
  // child ran. Returns whether a rehash is still under way.
  bool RehashIncrementally(int64_t budget_microseconds);
  // One step of active expiration: samples up to `max_samples` keys that
  // have a TTL, carrying on from where the previous call stopped, and
  // deletes those that have expired by `now`, a time in milliseconds.
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
  void SetCommandTime(int64_t now_milliseconds) {
    time_pinned_ = true;
    command_time_ = now_milliseconds;
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
  // A keyspace entry stores its key's object, which also holds the key's
  // TTL, after the entry's header and before the key's name, as Redis 8
  // keeps a key, its value, and its expiration in one block (kvobj): one
  // allocation per key, and a lookup that finds the key has its TTL too.
  using Keyspace = containers::Dict<std::string, RedisObject>;
  using KeyRef = Keyspace::EntryRef;
  struct KeyRefHash {
    size_t operator()(const KeyRef& key) const { return key.StoredHash(); }
  };
  // The live entry for `key`, empty if the key is missing. Deletes the key
  // if it has expired.
  KeyRef FindLive(const containers::HashedKey& key);
  // Gives the key a TTL, or replaces the one it has.
  void SetExpire(KeyRef entry, int64_t expire);
  // The expiration time of a key's object, 0 for none.
  int64_t ExpireOf(const RedisObject& object) const {
    const int64_t expire = object.Expire();
    return expire == RedisObject::kMaxExpire ? FarExpireOf(object) : expire;
  }
  int64_t FarExpireOf(const RedisObject& object) const;
  // Removes the key's TTL. Returns whether it had one.
  bool RemoveExpire(KeyRef entry);
  // Deletes a key found by its entry, as active expiration finds keys.
  void DeleteStoredKey(KeyRef entry);

  friend class aof::Aof;
  friend bool rdb::LoadRdb(int fd, RedisDb* db, const rdb::LoadOptions& options,
                           rdb::LoadStats* stats);
  RedisDb();
  void SetLoading(bool loading) { loading_ = loading; }
  // Each public operation hashes its key once and passes the HashedKey to
  // the keyspace and WATCH tables, which share one hash seed.
  RedisObject* Lookup(const containers::HashedKey& key);
  // Finds `key`'s keyspace entry, adding it if missing, without checking
  // whether an existing key has expired.
  KeySlot FindOrInsert(const containers::HashedKey& key);
  // TTL and WATCH bookkeeping shared by the setters once the value is
  // stored in `slot`.
  void FinishSet(const KeySlot& slot, int64_t expire, ExistingTtl existing_ttl);
  DbStatus Delete(const containers::HashedKey& key);
  void TouchWatchedKey(const containers::HashedKey& key);
  // Reads the object's own field: a time kept in far_expires_ is stored
  // there as kMaxExpire, which is later than any clock reading.
  bool IsExpired(const RedisObject& object) const {
    const int64_t expire = object.Expire();
    return expire != 0 && !loading_ && expire <= Now();
  }
  // Reads the clock, and keeps the time if it is pinned.
  int64_t ReadClock() const;

  std::unique_ptr<Keyspace> dict_;
  // The keys that have a TTL, so active expiration samples only those, as
  // Redis' expires dict holds a pointer to each such key's object. A key's
  // entry is in the set exactly while its object's Expire() is not 0, and
  // leaves it before the entry is freed.
  containers::HandleSet<KeyRef, KeyRefHash> expires_;
  // The exact expiration times from RedisObject::kMaxExpire on, by object
  // address. PEXPIREAT accepts any int64_t and the time must read back and
  // be saved unchanged, but an object has 61 bits for it. Normally empty.
  std::unique_ptr<containers::Dict<const RedisObject*, int64_t>> far_expires_;
  // Keyspaces a flush detached, waiting to be freed.
  std::deque<std::unique_ptr<Keyspace>> flushed_;
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
// follows, as Redis passes a dictEntryLink from lookupKeyWriteWithLink to
// setKeyByLink. The write does not hash the key again and, when
// FindOrCreateKey located it, does not probe the keyspace again. Valid until
// the key is deleted or the database is flushed, and no longer than the
// key's bytes.
class RedisDb::KeySlot {
 public:
  RedisObject* Object() const { return entry_ ? &entry_.Value() : nullptr; }
  // Whether FindOrCreateKey added the key.
  bool Created() const { return created_; }

 private:
  friend class RedisDb;
  KeySlot(const containers::HashedKey& key, KeyRef entry, bool created)
      : key_(key), entry_(entry), created_(created) {}

  containers::HashedKey key_;
  // Empty when the key is missing.
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
          if (keep_visiting && !IsExpired(object)) {
            const int64_t expire = ExpireOf(object);
            keep_visiting = visitor(
                std::string_view(key), object,
                expire == 0 ? std::nullopt : std::optional<int64_t>(expire));
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
  return dict_->ScanStep(
      cursor, count,
      [this, &visitor](std::string_view key, const RedisObject& object) {
        if (!IsExpired(object)) {
          visitor(key, object);
        }
      });
}
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_DB_H_
