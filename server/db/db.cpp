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
#include "utils/time_utils.h"

namespace redis_simple::db {
using in_memory::HashedKey;

std::unique_ptr<RedisDb> RedisDb::Create() {
  return std::unique_ptr<RedisDb>(new RedisDb());
}

RedisDb::RedisDb()
    : dict_(in_memory::Dict<std::string, RedisObjectPtr>::Create()),
      expires_(in_memory::Dict<std::string, int64_t>::Create()),
      watched_keys_(
          in_memory::Dict<std::string, std::vector<KeyWatcher*>>::Create()) {}

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
  return result->get();
}

DbStatus RedisDb::SetKey(std::string_view key, RedisObjectPtr object,
                         int64_t expire, ExistingTtl existing_ttl) {
  if (object == nullptr) {
    return DbStatus::kError;
  }
  // FindOrInsert copies the key only for a new entry.
  const HashedKey hashed(key);
  *dict_->FindOrInsert(hashed).first = std::move(object);
  FinishSet(hashed, expire, existing_ttl);
  return DbStatus::kOk;
}

DbStatus RedisDb::SetStringKey(std::string_view key, std::string_view value,
                               int64_t expire, ExistingTtl existing_ttl) {
  const HashedKey hashed(key);
  RedisObjectPtr& slot = *dict_->FindOrInsert(hashed).first;
  if (slot != nullptr && slot->Type() == RedisObject::ObjectType::kString) {
    slot->AssignString(value);
  } else {
    slot = RedisObject::CreateWithStringCopy(value);
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
    *expires_->FindOrInsert(key).first = expire;
    RS_LOG_DEBUG("add expire %" PRId64 "\n", expire);
  }
}

DbStatus RedisDb::DeleteKey(std::string_view key) {
  return Delete(HashedKey(key));
}

DbStatus RedisDb::Delete(const HashedKey& key) {
  if (!dict_->Delete(key)) {
    return DbStatus::kError;
  }
  // `key` may view the TTL entry's own key (active expiration), so touch
  // watchers before that entry is deleted.
  TouchWatchedKey(key);
  if (expires_->Size() > 0) {
    expires_->Delete(key);
  }
  return DbStatus::kOk;
}

DbStatus RedisDb::UnlinkKey(std::string_view key) {
  const HashedKey hashed(key);
  if (IsKeyExpired(hashed)) {
    Delete(hashed);
    return DbStatus::kError;
  }
  auto object = dict_->Extract(hashed);
  if (!object.has_value()) {
    return DbStatus::kError;
  }
  if (expires_->Size() > 0) {
    expires_->Delete(hashed);
  }
  TouchWatchedKey(hashed);
  return reclaimer_.Reclaim(std::move(*object)) ? DbStatus::kOk
                                                : DbStatus::kError;
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
  *expires_->FindOrInsert(hashed).first = expire;
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
  std::optional<RedisObjectPtr> object = dict_->Extract(old_hashed);
  if (!object.has_value()) {
    return DbStatus::kError;
  }
  const std::optional<int64_t> expire =
      expires_->Size() > 0 ? expires_->Extract(old_hashed) : std::nullopt;
  const HashedKey new_hashed(new_key);
  *dict_->FindOrInsert(new_hashed).first = std::move(*object);
  if (expire.has_value()) {
    *expires_->FindOrInsert(new_hashed).first = *expire;
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
  constexpr int64_t kMillisecondsPerSecond = 1000;
  constexpr int64_t kRoundingOffset = kMillisecondsPerSecond / 2;
  return (ttl / kMillisecondsPerSecond) +
         (ttl % kMillisecondsPerSecond >= kRoundingOffset ? 1 : 0);
}

void RedisDb::Flush() {
  if (HasWatchedKeys()) {
    // Like Redis, only watched keys that held a value are modified by FLUSHDB.
    std::optional<size_t> cursor = 0;
    while (cursor.has_value()) {
      cursor = watched_keys_->Scan(
          *cursor, [this](const std::string& key,
                          const std::vector<KeyWatcher*>& watchers) {
            if (dict_->FindValue(std::string_view(key)) != nullptr) {
              for (KeyWatcher* const watcher : watchers) {
                watcher->dirty_ = true;
              }
            }
          });
    }
  }
  dict_->Clear();
  expires_->Clear();
  expire_cursor_ = 0;
}

ExpireSampleResult RedisDb::ExpireSome(size_t max_samples, int64_t now) {
  ExpireSampleResult result;
  if (max_samples == 0 || expires_->Size() == 0) {
    return result;
  }

  std::vector<std::string_view> expired_keys;
  expired_keys.reserve(max_samples);
  bool scan_complete = false;
  while (result.sampled < max_samples && !scan_complete) {
    const std::optional<size_t> next_cursor = expires_->Scan(
        expire_cursor_, [&result, &expired_keys, max_samples, now](
                            const std::string& key, const int64_t& expire) {
          if (result.sampled >= max_samples) {
            return;
          }
          ++result.sampled;
          if (expire <= now) {
            expired_keys.push_back(key);
          }
        });
    scan_complete = !next_cursor.has_value();
    expire_cursor_ = next_cursor.value_or(0);
  }
  for (const auto& key : expired_keys) {
    if (DeleteKey(key) == DbStatus::kOk) {
      ++result.expired;
    }
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

int64_t RedisDb::Now() const {
  return command_time_.has_value() ? *command_time_
                                   : utils::NowInMilliseconds();
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
