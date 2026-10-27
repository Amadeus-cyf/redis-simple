#ifndef REDIS_SIMPLE_SERVER_DB_DB_H_
#define REDIS_SIMPLE_SERVER_DB_DB_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "memory/dict.h"
#include "server/db/async_reclaimer.h"
#include "server/db/redis_obj.h"

namespace redis_simple::aof {
class Aof;
}

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
  DbStatus DeleteKey(std::string_view key);
  DbStatus UnlinkKey(std::string_view key);
  DbStatus ExpireKeyAt(std::string_view key, int64_t expire,
                       ExpireCondition condition = ExpireCondition::kNone);
  DbStatus PersistKey(std::string_view key);
  DbStatus RenameKey(std::string_view old_key, std::string_view new_key);
  std::optional<int64_t> Expiration(std::string_view key) const;
  int64_t TimeToLive(std::string_view key, TtlResolution resolution);
  size_t KeyCount() const { return dict_->Size(); }
  // Key views and object references remain valid only until the database is
  // mutated. Returning false from the visitor stops further callbacks.
  template <typename Visitor>
  bool ForEachObject(Visitor&& visitor);
  // Key views remain valid until the database is mutated.
  template <typename Visitor>
  size_t ScanKeys(size_t cursor, size_t bucket_count, Visitor&& visitor);
  void Flush();
  ExpireSampleResult ExpireSome(size_t max_samples, int64_t now);
  // Pins the clock used for expiration while one command runs, so the
  // command sees a single consistent time and reads the clock only once.
  void SetCommandTime(int64_t now_ms) { command_time_ = now_ms; }
  void ClearCommandTime() { command_time_.reset(); }

 private:
  friend class aof::Aof;
  RedisDb();
  void SetLoading(bool loading) { loading_ = loading; }
  bool IsKeyExpired(std::string_view key) const;
  int64_t Now() const;
  std::unique_ptr<in_memory::Dict<std::string, RedisObjectPtr>> dict_;
  std::unique_ptr<in_memory::Dict<std::string, int64_t>> expires_;
  AsyncReclaimer reclaimer_;
  size_t expire_cursor_{};
  std::optional<int64_t> command_time_;
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
size_t RedisDb::ScanKeys(size_t cursor, size_t bucket_count,
                         Visitor&& visitor) {
  if (bucket_count == 0 || dict_->Size() == 0) {
    return 0;
  }

  std::optional<size_t> next_cursor = cursor;
  for (size_t scanned = 0; scanned < bucket_count && next_cursor.has_value();
       ++scanned) {
    next_cursor = dict_->Scan(
        *next_cursor,
        [this, &visitor](const std::string& key, const RedisObjectPtr&) {
          if (!IsKeyExpired(key)) {
            visitor(std::string_view(key));
          }
        });
  }
  return next_cursor.value_or(0);
}
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_DB_H_
