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
std::unique_ptr<RedisDb> RedisDb::Create() {
  return std::unique_ptr<RedisDb>(new RedisDb());
}

RedisDb::RedisDb()
    : dict_(in_memory::Dict<std::string, RedisObjectPtr>::Create()),
      expires_(in_memory::Dict<std::string, int64_t>::Create()) {}

const RedisObject* RedisDb::LookupKey(std::string_view key) {
  return MutableLookupKey(key);
}

RedisObject* RedisDb::MutableLookupKey(std::string_view key) {
  auto* const result = dict_->FindValue(key);
  if (result == nullptr) {
    return nullptr;
  }
  RedisObject* object = result->get();
  if (IsKeyExpired(key)) {
    RS_LOG_DEBUG("look up key expired\n");
    // If key is already expired, delete the key and return a null pointer.
    object = nullptr;
    DeleteKey(key);
  }
  return object;
}

DbStatus RedisDb::SetKey(std::string_view key, RedisObjectPtr object,
                         int64_t expire, ExistingTtl existing_ttl) {
  if (object == nullptr) {
    return DbStatus::kError;
  }
  // FindOrInsert hashes once and copies the key only for a new entry.
  *dict_->FindOrInsert(key).first = std::move(object);
  if (existing_ttl == ExistingTtl::kClear && expire == 0 &&
      expires_->Size() > 0) {
    expires_->Delete(key);
  }
  if (expire > 0) {
    *expires_->FindOrInsert(key).first = expire;
    RS_LOG_DEBUG("add expire %" PRId64 "\n", expire);
  }
  return DbStatus::kOk;
}

DbStatus RedisDb::DeleteKey(std::string_view key) {
  if (!dict_->Delete(key)) {
    return DbStatus::kError;
  }
  if (expires_->Size() > 0) {
    expires_->Delete(key);
  }
  return DbStatus::kOk;
}

DbStatus RedisDb::UnlinkKey(std::string_view key) {
  if (IsKeyExpired(key)) {
    DeleteKey(key);
    return DbStatus::kError;
  }
  auto object = dict_->Extract(key);
  if (!object.has_value()) {
    return DbStatus::kError;
  }
  expires_->Delete(key);
  return reclaimer_.Reclaim(std::move(*object)) ? DbStatus::kOk
                                                : DbStatus::kError;
}

DbStatus RedisDb::ExpireKeyAt(std::string_view key, int64_t expire,
                              ExpireCondition condition) {
  if (LookupKey(key) == nullptr) {
    return DbStatus::kError;
  }
  const auto current_expire = Expiration(key);
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
    }
    return false;
  }();
  if (!condition_met) {
    return DbStatus::kError;
  }
  if (!loading_ && expire <= Now()) {
    return DeleteKey(key);
  }
  *expires_->FindOrInsert(key).first = expire;
  return DbStatus::kOk;
}

std::optional<int64_t> RedisDb::Expiration(std::string_view key) const {
  const auto* expire = expires_->FindValue(key);
  return expire == nullptr ? std::nullopt : std::optional<int64_t>(*expire);
}

DbStatus RedisDb::PersistKey(std::string_view key) {
  if (LookupKey(key) == nullptr || expires_->FindValue(key) == nullptr) {
    return DbStatus::kError;
  }
  return expires_->Delete(key) ? DbStatus::kOk : DbStatus::kError;
}

DbStatus RedisDb::RenameKey(std::string_view old_key,
                            std::string_view new_key) {
  if (LookupKey(old_key) == nullptr) {
    return DbStatus::kError;
  }
  if (old_key == new_key) {
    return DbStatus::kOk;
  }

  // Extract moves the value and TTL out with one hash lookup each, instead of
  // finding and then deleting them separately.
  std::optional<RedisObjectPtr> object = dict_->Extract(old_key);
  if (!object.has_value()) {
    return DbStatus::kError;
  }
  const std::optional<int64_t> expire =
      expires_->Size() > 0 ? expires_->Extract(old_key) : std::nullopt;
  *dict_->FindOrInsert(new_key).first = std::move(*object);
  if (expire.has_value()) {
    *expires_->FindOrInsert(new_key).first = *expire;
  } else if (expires_->Size() > 0) {
    expires_->Delete(new_key);
  }
  return DbStatus::kOk;
}

int64_t RedisDb::TimeToLive(std::string_view key, TtlResolution resolution) {
  if (LookupKey(key) == nullptr) {
    return -2;
  }
  const auto* expire = expires_->FindValue(key);
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
    const auto next_cursor = expires_->Scan(
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

int64_t RedisDb::Now() const {
  return command_time_.has_value() ? *command_time_
                                   : utils::NowInMilliseconds();
}

bool RedisDb::IsKeyExpired(std::string_view key) const {
  if (loading_ || expires_->Size() == 0) {
    return false;
  }
  const auto* result = expires_->FindValue(key);
  if (result == nullptr) {
    return false;
  }
  return *result <= Now();
}
}  // namespace redis_simple::db
