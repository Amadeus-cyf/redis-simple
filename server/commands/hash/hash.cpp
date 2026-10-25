#include "data_types/hash/hash.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple::command::hashes {
namespace {
using Hash = ::redis_simple::hash::Hash;
constexpr auto kHash = db::RedisObject::ObjectType::kHash;

// Looks up the hash at `key`, replying WRONGTYPE when another type is there.
// Returns false when the command must stop.
bool LookupHash(Client* const client, std::string_view key,
                const Hash** const hash) {
  const auto [object, wrong_type] = LookupTyped(client->Db(), key, kHash);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return false;
  }
  *hash = object == nullptr ? nullptr : object->Hash();
  return true;
}

// Like LookupHash, but creates an empty hash when the key is missing.
bool LookupOrCreateHash(Client* const client, std::string_view key,
                        Hash** const hash) {
  auto [object, wrong_type] = MutableLookupTyped(client->Db(), key, kHash);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return false;
  }
  if (object == nullptr) {
    // LLVM 18 cannot trace unique_ptr ownership through Dict::FindOrInsert.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto created = db::RedisObject::CreateWithHash(Hash::Create());
    object = created.get();
    client->Db()->SetKey(key, std::move(created), 0);
  }
  *hash = object->Hash();
  return true;
}

bool AddWithOverflowCheck(int64_t value, int64_t increment, int64_t* result) {
  if ((increment > 0 &&
       value > std::numeric_limits<int64_t>::max() - increment) ||
      (increment < 0 &&
       value < std::numeric_limits<int64_t>::min() - increment)) {
    return false;
  }
  *result = value + increment;
  return true;
}

// Replies with every field or value of the hash at the command's key.
template <typename Visit>
void AddHashListReply(Client* const client, Visit visit) {
  const Hash* hash = nullptr;
  if (!LookupHash(client, client->Args()[0], &hash)) {
    return;
  }
  if (hash == nullptr) {
    client->AddReply(reply::FromArrayHeader(0));
    return;
  }
  std::string encoded = reply::FromArrayHeader(hash->Size());
  visit(*hash, [&encoded](std::string_view item) {
    reply::AppendBulkString(item, &encoded);
    return true;
  });
  client->AddReply(std::move(encoded));
}
}  // namespace

void HandleHSet(Client* const client) {
  const auto& args = client->Args();
  if (args.size() % 2 == 0) {
    client->AddReply(reply::kWrongNumberOfArguments);
    return;
  }
  Hash* hash = nullptr;
  if (!LookupOrCreateHash(client, args[0], &hash)) {
    return;
  }
  int64_t added = 0;
  for (size_t i = 1; i < args.size(); i += 2) {
    added += hash->Set(args[i], args[i + 1]) ? 1 : 0;
  }
  client->MarkModified();
  client->AddReply(reply::FromInt64(added));
}

void HandleHGet(Client* const client) {
  const auto& args = client->Args();
  const Hash* hash = nullptr;
  if (!LookupHash(client, args[0], &hash)) {
    return;
  }
  const bool found =
      hash != nullptr &&
      hash->VisitValue(args[1], [client](std::string_view value) {
        client->AddBulkReply(value);
      });
  if (!found) {
    client->AddReply(reply::NullView(client->Protocol()));
  }
}

void HandleHDel(Client* const client) {
  const auto& args = client->Args();
  const auto [object, wrong_type] =
      MutableLookupTyped(client->Db(), args[0], kHash);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  int64_t deleted = 0;
  if (object != nullptr) {
    Hash* const hash = object->Hash();
    for (size_t i = 1; i < args.size(); ++i) {
      deleted += hash->Delete(args[i]) ? 1 : 0;
    }
    if (hash->Size() == 0) {
      client->Db()->DeleteKey(args[0]);
    }
  }
  if (deleted > 0) {
    client->MarkModified();
  }
  client->AddReply(reply::FromInt64(deleted));
}

void HandleHLen(Client* const client) {
  const Hash* hash = nullptr;
  if (!LookupHash(client, client->Args()[0], &hash)) {
    return;
  }
  const size_t size = hash == nullptr ? 0 : hash->Size();
  client->AddReply(reply::FromInt64(static_cast<int64_t>(size)));
}

void HandleHExists(Client* const client) {
  const auto& args = client->Args();
  const Hash* hash = nullptr;
  if (!LookupHash(client, args[0], &hash)) {
    return;
  }
  const bool exists = hash != nullptr && hash->Exists(args[1]);
  client->AddReply(reply::FromInt64(exists ? 1 : 0));
}

void HandleHGetAll(Client* const client) {
  const Hash* hash = nullptr;
  if (!LookupHash(client, client->Args()[0], &hash)) {
    return;
  }
  const size_t size = hash == nullptr ? 0 : hash->Size();
  std::string encoded = reply::FromMapHeader(size, client->Protocol());
  if (hash != nullptr) {
    hash->ForEachEntry(
        [&encoded](std::string_view field, std::string_view value) {
          reply::AppendBulkString(field, &encoded);
          reply::AppendBulkString(value, &encoded);
          return true;
        });
  }
  client->AddReply(std::move(encoded));
}

void HandleHMGet(Client* const client) {
  const auto& args = client->Args();
  const Hash* hash = nullptr;
  if (!LookupHash(client, args[0], &hash)) {
    return;
  }
  const std::string_view null_reply = reply::NullView(client->Protocol());
  std::string encoded = reply::FromArrayHeader(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) {
    const bool found =
        hash != nullptr &&
        hash->VisitValue(args[i], [&encoded](std::string_view value) {
          reply::AppendBulkString(value, &encoded);
        });
    if (!found) {
      encoded.append(null_reply);
    }
  }
  client->AddReply(std::move(encoded));
}

void HandleHKeys(Client* const client) {
  AddHashListReply(client, [](const Hash& hash, auto&& visitor) {
    hash.ForEachField(visitor);
  });
}

void HandleHVals(Client* const client) {
  AddHashListReply(client, [](const Hash& hash, auto&& visitor) {
    hash.ForEachValue(visitor);
  });
}

void HandleHIncrBy(Client* const client) {
  const auto& args = client->Args();
  int64_t increment = 0;
  if (!utils::ToInt64(args[2], &increment)) {
    client->AddReply(
        reply::FromError("ERR value is not an integer or out of range"));
    return;
  }
  Hash* hash = nullptr;
  if (!LookupOrCreateHash(client, args[0], &hash)) {
    return;
  }
  int64_t current = 0;
  bool parsed = true;
  hash->VisitValue(args[1], [&current, &parsed](std::string_view value) {
    parsed = utils::ToInt64(value, &current);
  });
  int64_t next = 0;
  if (!parsed || !AddWithOverflowCheck(current, increment, &next)) {
    client->AddReply(
        reply::FromError("ERR hash value is not an integer or out of range"));
    return;
  }
  hash->Set(args[1], std::to_string(next));
  client->MarkModified();
  client->AddReply(reply::FromInt64(next));
}
}  // namespace redis_simple::command::hashes
