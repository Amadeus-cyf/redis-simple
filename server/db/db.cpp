#include "server/db/db.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "utils/check.h"
#include "utils/time.h"

namespace redis_simple::db {
using containers::HashedKey;

std::unique_ptr<RedisDb> RedisDb::Create() {
  return std::unique_ptr<RedisDb>(new RedisDb());
}

RedisDb::RedisDb()
    : dict_(Keyspace::Create()),
      expires_(ExpireTable::Create()),
      watched_keys_(
          containers::Dict<std::string, std::vector<KeyWatcher*>>::Create()) {}

const RedisObject* RedisDb::LookupKey(std::string_view key) {
  return MutableLookupKey(key);
}

RedisObject* RedisDb::MutableLookupKey(std::string_view key) {
  return Lookup(HashedKey(key));
}

RedisObject* RedisDb::Lookup(const HashedKey& key) {
  auto* const result = dict_->FindValue(key);
  if (result == nullptr) {
    return nullptr;
  }
  if (IsKeyExpired(key)) {
    RS_LOG_DEBUG("look up key expired\n");
    Delete(key);
    return nullptr;
  }
  return result;
}

DbStatus RedisDb::SetKey(std::string_view key, RedisObjectPtr object,
                         int64_t expire, ExistingTtl existing_ttl) {
  if (object == nullptr) {
    return DbStatus::kError;
  }
  // FindOrInsert copies the key only for a new entry.
  const HashedKey hashed(key);
  *dict_->FindOrInsert(hashed).first = std::move(*object);
  FinishSet(hashed, expire, existing_ttl);
  return DbStatus::kOk;
}

RedisObject* RedisDb::StoreKey(std::string_view key, RedisObjectPtr object) {
  RS_CHECK(object != nullptr);
  const HashedKey hashed(key);
  RedisObject* const stored = dict_->FindOrInsert(hashed).first;
  *stored = std::move(*object);
  FinishSet(hashed, 0, ExistingTtl::kClear);
  return stored;
}

bool RedisDb::AddKey(std::string_view key, RedisObjectPtr object,
                     int64_t expire) {
  const HashedKey hashed(key);
  const auto [slot, inserted] = dict_->FindOrInsert(hashed);
  if (!inserted) {
    return false;
  }
  *slot = std::move(*object);
  TouchWatchedKey(hashed);
  // A new key has no TTL to clear.
  if (expire > 0) {
    SetExpire(hashed, expire);
  }
  return true;
}

DbStatus RedisDb::SetStringKey(std::string_view key, std::string_view value,
                               int64_t expire, ExistingTtl existing_ttl) {
  const HashedKey hashed(key);
  const auto [slot, inserted] = dict_->FindOrInsert(hashed);
  if (!inserted && slot->Type() == RedisObject::ObjectType::kString) {
    slot->AssignString(value);
  } else {
    slot->ReplaceWithString(value);
  }
  FinishSet(hashed, expire, existing_ttl);
  return DbStatus::kOk;
}

void RedisDb::FinishSet(const HashedKey& key, int64_t expire,
                        ExistingTtl existing_ttl) {
  TouchWatchedKey(key);
  if (existing_ttl == ExistingTtl::kClear && expire == 0 &&
      expires_->Size() > 0) {
    expires_->Delete(key);
  }
  if (expire > 0) {
    SetExpire(key, expire);
    RS_LOG_DEBUG("add expire %" PRId64 "\n", expire);
  }
}

DbStatus RedisDb::DeleteKey(std::string_view key) {
  return Delete(HashedKey(key));
}

DbStatus RedisDb::Delete(const HashedKey& key) {
  // The TTL goes first: its entry reads the key from the keyspace entry.
  if (expires_->Size() > 0) {
    expires_->Delete(key);
  }
  if (!dict_->Delete(key)) {
    return DbStatus::kError;
  }
  TouchWatchedKey(key);
  return DbStatus::kOk;
}

void RedisDb::DeleteStoredKey(std::string_view key) {
  const HashedKey hashed(key);
  // `key` views the keyspace entry, so everything that reads it runs before
  // that entry is freed.
  TouchWatchedKey(hashed);
  expires_->Delete(hashed);
  dict_->Delete(hashed);
}

void RedisDb::SetExpire(const HashedKey& key, int64_t expire) {
  const KeyRef stored = dict_->FindEntryRef(key);
  if (!stored) {
    utils::Unreachable();  // Callers add the key to the keyspace first.
  }
  *expires_->FindOrInsert(stored).first = expire;
}

DbStatus RedisDb::UnlinkKey(std::string_view key) {
  const HashedKey hashed(key);
  if (IsKeyExpired(hashed)) {
    Delete(hashed);
    return DbStatus::kError;
  }
  if (expires_->Size() > 0) {
    expires_->Delete(hashed);
  }
  auto object = dict_->Extract(hashed);
  if (!object.has_value()) {
    return DbStatus::kError;
  }
  TouchWatchedKey(hashed);
  // Free a small value now, as Redis frees values with at most 64 elements
  // (LAZYFREE_THRESHOLD) at once; queue a larger one for FreeLazily.
  constexpr size_t kFreeAtOnceElements = 64;
  if (!object->ReleaseSome(kFreeAtOnceElements)) {
    unlinked_.push_back(std::move(*object));
  }
  return DbStatus::kOk;
}

DbStatus RedisDb::ExpireKeyAt(std::string_view key, int64_t expire,
                              ExpireCondition condition) {
  const HashedKey hashed(key);
  if (Lookup(hashed) == nullptr) {
    return DbStatus::kError;
  }
  const int64_t* const existing = expires_->FindValue(hashed);
  const std::optional<int64_t> current_expire =
      existing == nullptr ? std::nullopt : std::optional<int64_t>(*existing);
  const bool condition_met = [&] {
    switch (condition) {
      case ExpireCondition::kNone:
        return true;
      case ExpireCondition::kIfNoExpiry:
        return !current_expire.has_value();
      case ExpireCondition::kIfHasExpiry:
        return current_expire.has_value();
      case ExpireCondition::kGreaterThan:
        return current_expire.has_value() && expire > *current_expire;
      case ExpireCondition::kLessThan:
        return !current_expire.has_value() || expire < *current_expire;
      case ExpireCondition::kIfHasExpiryAndLessThan:
        return current_expire.has_value() && expire < *current_expire;
    }
    return false;
  }();
  if (!condition_met) {
    return DbStatus::kError;
  }
  if (!loading_ && expire <= Now()) {
    return Delete(hashed);
  }
  SetExpire(hashed, expire);
  TouchWatchedKey(hashed);
  return DbStatus::kOk;
}

std::optional<int64_t> RedisDb::Expiration(std::string_view key) const {
  const int64_t* expire = expires_->FindValue(key);
  return expire == nullptr ? std::nullopt : std::optional<int64_t>(*expire);
}

RedisDb::KeyWithExpiration RedisDb::LookupKeyWithExpiration(
    std::string_view key) {
  const HashedKey hashed(key);
  KeyWithExpiration result;
  result.object = Lookup(hashed);
  if (result.object != nullptr) {
    if (const int64_t* const expire = expires_->FindValue(hashed);
        expire != nullptr) {
      result.expire = *expire;
    }
  }
  return result;
}

DbStatus RedisDb::PersistKey(std::string_view key) {
  const HashedKey hashed(key);
  // Delete fails when there is no TTL, so it doubles as the existence check.
  if (Lookup(hashed) == nullptr || !expires_->Delete(hashed)) {
    return DbStatus::kError;
  }
  TouchWatchedKey(hashed);
  return DbStatus::kOk;
}

DbStatus RedisDb::RenameKey(std::string_view old_key,
                            std::string_view new_key) {
  const HashedKey old_hashed(old_key);
  if (Lookup(old_hashed) == nullptr) {
    return DbStatus::kError;
  }
  if (old_key == new_key) {
    return DbStatus::kOk;
  }

  // Extract moves the value and TTL out with one bucket walk each, instead of
  // finding and then deleting them separately.
  // The TTL comes out first: its entry reads the key from the keyspace entry.
  const std::optional<int64_t> expire =
      expires_->Size() > 0 ? expires_->Extract(old_hashed) : std::nullopt;
  std::optional<RedisObject> object = dict_->Extract(old_hashed);
  if (!object.has_value()) {
    return DbStatus::kError;
  }
  const HashedKey new_hashed(new_key);
  // An existing key under the new name keeps its entry; its TTL is replaced.
  *dict_->FindOrInsert(new_hashed).first = std::move(*object);
  if (expire.has_value()) {
    SetExpire(new_hashed, *expire);
  } else if (expires_->Size() > 0) {
    expires_->Delete(new_hashed);
  }
  TouchWatchedKey(old_hashed);
  TouchWatchedKey(new_hashed);
  return DbStatus::kOk;
}

int64_t RedisDb::TimeToLive(std::string_view key, TtlResolution resolution) {
  const HashedKey hashed(key);
  if (Lookup(hashed) == nullptr) {
    return -2;
  }
  const int64_t* expire = expires_->FindValue(hashed);
  if (expire == nullptr) {
    return -1;
  }
  const int64_t ttl = std::max<int64_t>(*expire - Now(), 0);
  if (resolution == TtlResolution::kMilliseconds) {
    return ttl;
  }
  constexpr int64_t kRoundingOffset = utils::kMillisecondsPerSecond / 2;
  return (ttl / utils::kMillisecondsPerSecond) +
         (ttl % utils::kMillisecondsPerSecond >= kRoundingOffset ? 1 : 0);
}

void RedisDb::Flush() {
  if (HasWatchedKeys()) {
    // Like Redis, only watched keys that held a value are modified by FLUSHDB.
    std::optional<size_t> cursor = 0;
    while (cursor.has_value()) {
      cursor = watched_keys_->Scan(
          *cursor, [this](std::string_view key,
                          const std::vector<KeyWatcher*>& watchers) {
            if (dict_->FindValue(key) != nullptr) {
              for (KeyWatcher* const watcher : watchers) {
                watcher->dirty_ = true;
              }
            }
          });
    }
  }
  expire_cursor_ = 0;
  // Freeing a keyspace takes about 70 ms per million keys, during which no
  // client would be served. Redis 8 frees a flushed database on a background
  // thread; here memory freed by another thread stays with mimalloc until
  // this thread collects it, which costs about as much as freeing it, so a
  // large keyspace is detached and freed on this thread in budgeted steps.
  constexpr size_t kFlushInPlaceKeys = 4096;
  if (dict_->Size() <= kFlushInPlaceKeys) {
    expires_->Clear();
    dict_->Clear();
    return;
  }
  flushed_.push_back({.expires = std::exchange(expires_, ExpireTable::Create()),
                      .keyspace = std::exchange(dict_, Keyspace::Create())});
}

bool RedisDb::FreeLazily(int64_t budget_milliseconds) {
  // Enough entries that reading the clock costs little, few enough that the
  // budget is kept to within a fraction of a millisecond.
  constexpr size_t kEntriesPerStep = 1024;
  if (unlinked_.empty() && flushed_.empty()) {
    return false;
  }
  const int64_t start = utils::MonotonicMilliseconds();
  while (!unlinked_.empty() || !flushed_.empty()) {
    if (!unlinked_.empty()) {
      if (unlinked_.front().ReleaseSome(kEntriesPerStep)) {
        unlinked_.pop_front();
      }
    } else {
      FlushedTables& tables = flushed_.front();
      if (tables.expires->ClearSome(kEntriesPerStep) &&
          tables.keyspace->ClearSome(kEntriesPerStep)) {
        flushed_.pop_front();
      }
    }
    if (utils::MonotonicMilliseconds() - start >= budget_milliseconds) {
      break;
    }
  }
  return !unlinked_.empty() || !flushed_.empty();
}

ExpireSampleResult RedisDb::ExpireSome(size_t max_samples, int64_t now) {
  ExpireSampleResult result;
  if (max_samples == 0 || expires_->Size() == 0) {
    return result;
  }

  std::vector<std::string_view> expired_keys;
  expired_keys.reserve(max_samples);
  // A scan resumed near the end of the table can finish without meeting a
  // key. It then starts over once, so that only an empty table yields an
  // empty sample; no key is visited twice, since the first pass visited none.
  bool may_start_over = expire_cursor_ != 0;
  while (result.sampled < max_samples) {
    // Every key of a visited bucket is sampled, even past `max_samples`:
    // the cursor has moved on, so a key left out here would be left out of
    // every later pass over an unchanged table too.
    const std::optional<size_t> next_cursor = expires_->Scan(
        expire_cursor_, [&result, &expired_keys, now](const KeyRef& key,
                                                      const int64_t& expire) {
          ++result.sampled;
          if (expire <= now) {
            expired_keys.push_back(key.Key());
          }
        });
    expire_cursor_ = next_cursor.value_or(0);
    if (next_cursor.has_value()) {
      continue;
    }
    if (result.sampled > 0 || !may_start_over) {
      break;
    }
    may_start_over = false;
  }
  // Each key views its own keyspace entry, which deleting it frees; the
  // others stay valid, since entries never move.
  for (const std::string_view key : expired_keys) {
    DeleteStoredKey(key);
    ++result.expired;
  }
  return result;
}

void RedisDb::WatchKey(std::string_view key, KeyWatcher* const watcher) {
  if (std::ranges::find(watcher->keys_, key) != watcher->keys_.end()) {
    return;
  }
  // Resolve lazy expiration first so a key that already expired does not
  // count as a modification made after WATCH.
  LookupKey(key);
  watched_keys_->FindOrInsert(key).first->push_back(watcher);
  watcher->keys_.emplace_back(key);
}

void RedisDb::UnwatchKeys(KeyWatcher* const watcher) {
  for (const std::string& key : watcher->keys_) {
    auto* const watchers = watched_keys_->FindValue(std::string_view(key));
    if (watchers == nullptr) {
      continue;
    }
    std::erase(*watchers, watcher);
    if (watchers->empty()) {
      watched_keys_->Delete(std::string_view(key));
    }
  }
  watcher->keys_.clear();
  watcher->dirty_ = false;
}

void RedisDb::TouchWatchedKey(std::string_view key) {
  if (watched_keys_->Size() == 0) {
    return;
  }
  TouchWatchedKey(HashedKey(key));
}

void RedisDb::TouchWatchedKey(const HashedKey& key) {
  if (watched_keys_->Size() == 0) {
    return;
  }
  auto* const watchers = watched_keys_->FindValue(key);
  if (watchers == nullptr) {
    return;
  }
  for (KeyWatcher* const watcher : *watchers) {
    watcher->dirty_ = true;
  }
}

int64_t RedisDb::ReadClock() const {
  const int64_t now = utils::NowInMilliseconds();
  if (time_pinned_) {
    command_time_ = now;
  }
  return now;
}

bool RedisDb::IsKeyExpired(std::string_view key) const {
  if (loading_ || expires_->Size() == 0) {
    return false;
  }
  return IsKeyExpired(HashedKey(key));
}

bool RedisDb::IsKeyExpired(const HashedKey& key) const {
  if (loading_ || expires_->Size() == 0) {
    return false;
  }
  const int64_t* result = expires_->FindValue(key);
  if (result == nullptr) {
    return false;
  }
  return *result <= Now();
}
}  // namespace redis_simple::db
