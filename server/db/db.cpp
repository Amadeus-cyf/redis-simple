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
      far_expires_(containers::Dict<const RedisObject*, int64_t>::Create()),
      watched_keys_(
          containers::Dict<std::string, std::vector<KeyWatcher*>>::Create()) {}

const RedisObject* RedisDb::LookupKey(std::string_view key) {
  return MutableLookupKey(key);
}

RedisObject* RedisDb::MutableLookupKey(std::string_view key) {
  return Lookup(HashedKey(key));
}

RedisObject* RedisDb::Lookup(const HashedKey& key) {
  const KeyRef entry = FindLive(key);
  return entry ? &entry.Value() : nullptr;
}

RedisDb::KeyRef RedisDb::FindLive(const HashedKey& key) {
  const KeyRef entry = dict_->FindEntryRef(key);
  if (entry && IsExpired(entry.Value())) {
    RS_LOG_DEBUG("look up key expired\n");
    Delete(key);
    return {};
  }
  return entry;
}

RedisDb::KeySlot RedisDb::FindKeyForWrite(std::string_view key) {
  const HashedKey hashed(key);
  return {hashed, FindLive(hashed), false};
}

RedisDb::KeySlot RedisDb::FindOrCreateKey(std::string_view key) {
  const HashedKey hashed(key);
  KeySlot slot = FindOrInsert(hashed);
  if (!slot.created_ && IsExpired(slot.entry_.Value())) {
    RS_LOG_DEBUG("look up key expired\n");
    Delete(hashed);
    slot = FindOrInsert(hashed);
  }
  if (slot.created_) {
    TouchWatchedKey(hashed);
  }
  return slot;
}

RedisDb::KeySlot RedisDb::FindOrInsert(const HashedKey& key) {
  // FindOrInsertEntry copies the key only for a new entry.
  const Keyspace::InsertResult found = dict_->FindOrInsertEntry(key);
  return {key, found.entry, found.inserted};
}

DbStatus RedisDb::SetKey(std::string_view key, RedisObjectPtr object,
                         int64_t expire, ExistingTtl existing_ttl) {
  if (object == nullptr) {
    return DbStatus::kError;
  }
  const KeySlot slot = FindOrInsert(HashedKey(key));
  *slot.Object() = std::move(*object);
  FinishSet(slot, expire, existing_ttl);
  return DbStatus::kOk;
}

bool RedisDb::AddKey(std::string_view key, RedisObjectPtr object,
                     int64_t expire) {
  const KeySlot slot = FindOrInsert(HashedKey(key));
  if (!slot.created_) {
    return false;
  }
  *slot.Object() = std::move(*object);
  FinishSet(slot, expire, ExistingTtl::kClear);
  return true;
}

void RedisDb::ReserveKeys(size_t keys, size_t keys_with_expiration) {
  dict_->Reserve(dict_->Size() + keys);
  expires_.Reserve(expires_.Size() + keys_with_expiration);
}

DbStatus RedisDb::SetStringKey(std::string_view key, std::string_view value,
                               int64_t expire, ExistingTtl existing_ttl) {
  SetStringAt(FindOrInsert(HashedKey(key)), value, expire, existing_ttl);
  return DbStatus::kOk;
}

void RedisDb::SetStringAt(const KeySlot& slot, std::string_view value,
                          int64_t expire, ExistingTtl existing_ttl) {
  RedisObject* const object = slot.Object();
  RS_CHECK(object != nullptr);
  if (!slot.created_ && object->Type() == RedisObject::ObjectType::kString) {
    object->AssignString(value);
  } else {
    object->ReplaceWithString(value);
  }
  FinishSet(slot, expire, existing_ttl);
}

void RedisDb::FinishSet(const KeySlot& slot, int64_t expire,
                        ExistingTtl existing_ttl) {
  TouchWatchedKey(slot.key_);
  if (expire > 0) {
    SetExpire(slot.entry_, expire);
    RS_LOG_DEBUG("add expire %" PRId64 "\n", expire);
  } else if (existing_ttl == ExistingTtl::kClear) {
    RemoveExpire(slot.entry_);
  }
}

DbStatus RedisDb::DeleteKey(std::string_view key) {
  return Delete(HashedKey(key));
}

DbStatus RedisDb::Delete(const HashedKey& key) {
  const bool deleted =
      dict_->DeleteWith(key, [this](KeyRef entry) { RemoveExpire(entry); });
  if (!deleted) {
    return DbStatus::kError;
  }
  TouchWatchedKey(key);
  return DbStatus::kOk;
}

void RedisDb::DeleteStoredKey(KeyRef entry) {
  // The key's bytes are in the entry, so the watchers are told before it
  // is freed.
  const HashedKey key = entry.Hashed();
  TouchWatchedKey(key);
  RemoveExpire(entry);
  dict_->Delete(key);
}

void RedisDb::SetExpire(KeyRef entry, int64_t expire) {
  RS_CHECK(entry);
  RedisObject& object = entry.Value();
  if (object.Expire() == 0) {
    expires_.Insert(entry);
  } else if (object.Expire() == RedisObject::kMaxExpire) {
    far_expires_->Delete(&object);
  }
  if (expire >= RedisObject::kMaxExpire) {
    far_expires_->Set(&object, expire);
    expire = RedisObject::kMaxExpire;
  }
  // 0 means no TTL, so a time at or before the epoch, which only a replayed
  // append-only file can ask for, is kept as the earliest time there is.
  object.SetExpire(std::max<int64_t>(expire, 1));
}

int64_t RedisDb::FarExpireOf(const RedisObject& object) const {
  const int64_t* const expire = far_expires_->FindValue(&object);
  RS_CHECK(expire != nullptr);
  return *expire;
}

bool RedisDb::RemoveExpire(KeyRef entry) {
  RedisObject& object = entry.Value();
  if (object.Expire() == 0) {
    return false;
  }
  if (object.Expire() == RedisObject::kMaxExpire) {
    far_expires_->Delete(&object);
  }
  expires_.Erase(entry);
  object.SetExpire(0);
  return true;
}

DbStatus RedisDb::UnlinkKey(std::string_view key) {
  const HashedKey hashed(key);
  const KeyRef entry = FindLive(hashed);
  if (!entry) {
    return DbStatus::kError;
  }
  RemoveExpire(entry);
  std::optional<RedisObject> object = dict_->Extract(hashed);
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
  const KeyRef entry = FindLive(hashed);
  if (!entry) {
    return DbStatus::kError;
  }
  const int64_t existing = ExpireOf(entry.Value());
  const std::optional<int64_t> current_expire =
      existing == 0 ? std::nullopt : std::optional<int64_t>(existing);
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
  SetExpire(entry, expire);
  TouchWatchedKey(hashed);
  return DbStatus::kOk;
}

std::optional<int64_t> RedisDb::Expiration(std::string_view key) {
  const KeyRef entry = dict_->FindEntryRef(key);
  if (!entry || entry.Value().Expire() == 0) {
    return std::nullopt;
  }
  return ExpireOf(entry.Value());
}

RedisDb::KeyWithExpiration RedisDb::LookupKeyWithExpiration(
    std::string_view key) {
  KeyWithExpiration result;
  if (const KeyRef entry = FindLive(HashedKey(key))) {
    const RedisObject& object = entry.Value();
    result.object = &object;
    if (object.Expire() != 0) {
      result.expire = ExpireOf(object);
    }
  }
  return result;
}

DbStatus RedisDb::PersistKey(std::string_view key) {
  const HashedKey hashed(key);
  const KeyRef entry = FindLive(hashed);
  if (!entry || !RemoveExpire(entry)) {
    return DbStatus::kError;
  }
  TouchWatchedKey(hashed);
  return DbStatus::kOk;
}

DbStatus RedisDb::RenameKey(std::string_view old_key,
                            std::string_view new_key) {
  const HashedKey old_hashed(old_key);
  const KeyRef source = FindLive(old_hashed);
  if (!source) {
    return DbStatus::kError;
  }
  if (old_key == new_key) {
    return DbStatus::kOk;
  }

  const int64_t expire = ExpireOf(source.Value());
  RemoveExpire(source);
  // Extract moves the value out with one bucket walk, instead of finding and
  // then deleting it.
  std::optional<RedisObject> value = dict_->Extract(old_hashed);
  if (!value.has_value()) {
    return DbStatus::kError;
  }
  const HashedKey new_hashed(new_key);
  // An existing key under the new name keeps its entry; its TTL is replaced.
  const Keyspace::InsertResult target = dict_->FindOrInsertEntry(new_hashed);
  *target.value = std::move(*value);
  if (expire != 0) {
    SetExpire(target.entry, expire);
  } else {
    RemoveExpire(target.entry);
  }
  TouchWatchedKey(old_hashed);
  TouchWatchedKey(new_hashed);
  return DbStatus::kOk;
}

int64_t RedisDb::TimeToLive(std::string_view key, TtlResolution resolution) {
  const KeyRef entry = FindLive(HashedKey(key));
  if (!entry) {
    return -2;
  }
  const int64_t expire = ExpireOf(entry.Value());
  if (expire == 0) {
    return -1;
  }
  const int64_t ttl = std::max<int64_t>(expire - Now(), 0);
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
  // The TTL index is one array, freed at once whatever its size.
  expires_.Clear();
  far_expires_->Clear();
  if (dict_->Size() <= kFlushInPlaceKeys) {
    dict_->Clear();
    return;
  }
  flushed_.push_back(std::exchange(dict_, Keyspace::Create()));
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
    } else if (flushed_.front()->ClearSome(kEntriesPerStep)) {
      flushed_.pop_front();
    }
    if (utils::MonotonicMilliseconds() - start >= budget_milliseconds) {
      break;
    }
  }
  return true;
}

bool RedisDb::RehashIncrementally(int64_t budget_microseconds) {
  // As Redis' dictRehashMicroseconds: enough buckets that reading the clock
  // costs little, few enough to overrun the budget only slightly.
  constexpr int kBucketsPerStep = 100;
  dict_->ShrinkIfNeeded();
  expires_.ShrinkIfNeeded();
  if (!dict_->IsRehashing() && !expires_.IsRebuilding()) {
    return false;
  }
  const int64_t start = utils::MonotonicMicroseconds();
  // The keyspace table first, then the TTL index.
  while (dict_->RehashSome(kBucketsPerStep) ||
         expires_.RebuildSome(kBucketsPerStep)) {
    if (utils::MonotonicMicroseconds() - start >= budget_microseconds) {
      break;
    }
  }
  return true;
}

ExpireSampleResult RedisDb::ExpireSome(size_t max_samples, int64_t now) {
  ExpireSampleResult result;
  if (max_samples == 0 || expires_.Size() == 0) {
    return result;
  }

  std::vector<KeyRef> expired_keys;
  expired_keys.reserve(max_samples);
  // One call scans each position at most once: it carries on from the start
  // of the index when it reaches the end, and stops where it began.
  const size_t first_cursor = expire_cursor_;
  bool wrapped = false;
  while (result.sampled < max_samples) {
    const std::optional<size_t> next_cursor =
        expires_.Scan(expire_cursor_,
                      [&result, &expired_keys, max_samples, now](KeyRef entry) {
                        if (result.sampled >= max_samples) {
                          return;
                        }
                        ++result.sampled;
                        if (entry.Value().Expire() <= now) {
                          expired_keys.push_back(entry);
                        }
                      });
    expire_cursor_ = next_cursor.value_or(0);
    if (!next_cursor.has_value()) {
      if (wrapped) {
        break;
      }
      wrapped = true;
    }
    if (wrapped && expire_cursor_ >= first_cursor) {
      break;
    }
  }
  // Deleting a key frees only its own entry; the others stay valid, since
  // entries never move.
  for (const KeyRef entry : expired_keys) {
    DeleteStoredKey(entry);
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
}  // namespace redis_simple::db
