#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "fuzz/fuzz_input.h"
#include "server/db/db.h"
#include "server/db/redis_obj.h"

namespace redis_simple::fuzz {
namespace {
struct DbValue {
  std::string value;
  bool expires{};
};
using DbModel = std::unordered_map<std::string, DbValue>;

constexpr int64_t kPastExpiration = 1;
constexpr int64_t kFutureExpiration = std::numeric_limits<int64_t>::max();

db::RedisObjectPtr MakeObject(std::string value) {
  return db::RedisObject::CreateWithString(std::move(value));
}

void Verify(db::RedisDb* database, const DbModel& model) {
  Require(database->KeyCount() == model.size());
  for (const auto& [key, expected] : model) {
    const auto* object = database->LookupKey(key);
    Require(object != nullptr);
    Require(object->String() == expected.value);
    const int64_t ttl =
        database->TimeToLive(key, db::TtlResolution::kMilliseconds);
    Require(expected.expires ? ttl > 0 : ttl == -1);
  }
}

void SetValue(db::RedisDb* database, DbModel* model, const std::string& key,
              const std::string& value, bool expires, bool keep_ttl) {
  const db::ExistingTtl existing_ttl =
      keep_ttl ? db::ExistingTtl::kKeep : db::ExistingTtl::kClear;
  const int64_t expiration = expires ? kFutureExpiration : 0;
  Require(database->SetKey(key, MakeObject(value), expiration, existing_ttl) ==
          db::DbStatus::kOk);

  const auto existing = model->find(key);
  const bool retained_expiration =
      keep_ttl && existing != model->end() && existing->second.expires;
  (*model)[key] = {value, expires || retained_expiration};
}

void RenameValue(db::RedisDb* database, DbModel* model,
                 const std::string& old_key, const std::string& new_key) {
  const auto old = model->find(old_key);
  const bool exists = old != model->end();
  Require(database->RenameKey(old_key, new_key) ==
          (exists ? db::DbStatus::kOk : db::DbStatus::kError));
  if (!exists || old_key == new_key) {
    return;
  }
  DbValue value = std::move(old->second);
  model->erase(old);
  (*model)[new_key] = std::move(value);
}

void ExpireAll(db::RedisDb* database, DbModel* model) {
  size_t expected_expired = 0;
  for (auto it = model->begin(); it != model->end();) {
    if (it->second.expires) {
      it = model->erase(it);
      ++expected_expired;
    } else {
      ++it;
    }
  }
  const auto result =
      database->ExpireSome(database->KeyCount() + 1, kFutureExpiration);
  Require(result.expired == expected_expired);
}

void RunOperations(FuzzInput* input) {
  auto database = db::RedisDb::Create();
  DbModel model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 11;
    const std::string key = input->ReadValue(64);
    const std::string value = input->ReadValue(96);
    switch (operation) {
      case 0:
        SetValue(database.get(), &model, key, value, false, false);
        break;
      case 1:
        SetValue(database.get(), &model, key, value, true, false);
        break;
      case 2:
        SetValue(database.get(), &model, key, value, false, true);
        break;
      case 3:
        Require(
            database->DeleteKey(key) ==
            (model.erase(key) != 0 ? db::DbStatus::kOk : db::DbStatus::kError));
        break;
      case 4: {
        const auto it = model.find(key);
        const bool expected = it != model.end() && it->second.expires;
        Require(database->PersistKey(key) ==
                (expected ? db::DbStatus::kOk : db::DbStatus::kError));
        if (expected) {
          it->second.expires = false;
        }
        break;
      }
      case 5:
        RenameValue(database.get(), &model, key, input->ReadValue(64));
        break;
      case 6: {
        const bool future = (input->ReadByte() & 1U) != 0;
        const auto it = model.find(key);
        const bool exists = it != model.end();
        Require(database->ExpireKeyAt(
                    key, future ? kFutureExpiration : kPastExpiration) ==
                (exists ? db::DbStatus::kOk : db::DbStatus::kError));
        if (exists) {
          if (future) {
            it->second.expires = true;
          } else {
            model.erase(it);
          }
        }
        break;
      }
      case 7:
        ExpireAll(database.get(), &model);
        break;
      case 8:
        database->Flush();
        model.clear();
        break;
      case 9:
        Require(database->SetKey(key, MakeObject(value), kPastExpiration) ==
                db::DbStatus::kOk);
        Require(database->LookupKey(key) == nullptr);
        model.erase(key);
        break;
      case 10:
        Require(
            database->UnlinkKey(key) ==
            (model.erase(key) != 0 ? db::DbStatus::kOk : db::DbStatus::kError));
        break;
      default:
        break;
    }
    Verify(database.get(), model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
