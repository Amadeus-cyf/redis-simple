#include "data_types/hash/hash.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/commands/scan_options.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/strings.h"

namespace redis_simple::commands::hash {
namespace {
using Hash = ::redis_simple::data_types::Hash;
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

// Sets field-value pairs, as Redis' hsetCommand does for both HSET and
// HMSET. Returns the number of new fields, or nullopt after replying with an
// error.
std::optional<int64_t> SetFields(Client* const client,
                                 std::string_view command_name) {
  const commands::CommandArgs& args = client->Args();
  if (args.size() % 2 == 0) {
    client->AddReply(reply::WrongNumberOfArguments(command_name));
    return std::nullopt;
  }
  Hash* hash = nullptr;
  if (!LookupOrCreateHash(client, args[0], &hash)) {
    return std::nullopt;
  }
  int64_t added = 0;
  for (size_t i = 1; i < args.size(); i += 2) {
    added += hash->Set(args[i], args[i + 1]) ? 1 : 0;
  }
  // Redis counts every pair set, new or not.
  client->MarkModified(static_cast<int64_t>(args.size() / 2));
  return added;
}
}  // namespace

void HandleHSet(Client* const client) {
  if (const std::optional<int64_t> added = SetFields(client, "hset")) {
    client->AddReply(reply::FromInt64(*added));
  }
}

// HSET under its older name, which replies OK. Redis' AOF rewrite writes
// hashes as HMSET, so loading a Redis AOF needs it.
void HandleHMSet(Client* const client) {
  if (SetFields(client, "hmset").has_value()) {
    client->AddReply(reply::kOk);
  }
}

void HandleHGet(Client* const client) {
  const commands::CommandArgs& args = client->Args();
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
  const commands::CommandArgs& args = client->Args();
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
    client->MarkModified(deleted);
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
  const commands::CommandArgs& args = client->Args();
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

void HandleHScan(Client* const client) {
  size_t cursor = 0;
  ScanOptions options;
  db::RedisObject* const object =
      StartCollectionScan(client, ScanTarget::kHash, &cursor, &options);
  if (object == nullptr) {
    return;
  }
  Hash* const hash = object->Hash();
  const bool dict = hash->Encoding() == Hash::Encoding::kDict;
  ScanReply reply(2 * (dict ? options.count : hash->Size()));
  const auto add = [&options, &reply](std::string_view field,
                                      std::string_view value) {
    if (!options.Matches(field)) {
      return;
    }
    reply.Add(field);
    if (!options.no_values) {
      reply.Add(value);
    }
  };
  size_t next_cursor = 0;
  if (dict) {
    next_cursor = hash->ScanDict(cursor, options.count, add);
  } else {
    // A listpack is small, so Redis returns all of it at once, whatever the
    // cursor and COUNT.
    hash->ForEachEntry([&add](std::string_view field, std::string_view value) {
      add(field, value);
      return true;
    });
  }
  std::move(reply).Send(client, next_cursor);
}

void HandleHMGet(Client* const client) {
  const commands::CommandArgs& args = client->Args();
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
  const commands::CommandArgs& args = client->Args();
  int64_t increment = 0;
  if (!utils::ToInt64(args[2], &increment)) {
    client->AddReply(reply::kNotAnIntegerError);
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
  if (!parsed) {
    client->AddReply(reply::FromError("ERR hash value is not an integer"));
    return;
  }
  int64_t next = 0;
  if (!AddWithOverflowCheck(current, increment, &next)) {
    client->AddReply(reply::kOverflowError);
    return;
  }
  // Enough for INT64_MIN and a terminator.
  std::array<char, 21> buffer{};
  const int length = utils::Int64ToString(buffer, next);
  hash->Set(args[1],
            std::string_view(buffer.data(), static_cast<size_t>(length)));
  client->MarkModified();
  client->AddReply(reply::FromInt64(next));
}
}  // namespace redis_simple::commands::hash
