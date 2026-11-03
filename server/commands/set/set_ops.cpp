#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data_types/set/set.h"
#include "memory/dict.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/check.h"

namespace redis_simple::command::sets {
namespace {
using Set = ::redis_simple::set::Set;

enum class SetLookupStatus : std::uint8_t {
  kOk,
  kMissing,
  kWrongType,
};

struct SetLookup {
  const Set* set;
  SetLookupStatus status;
};

struct SetReply {
  size_t count{};
  std::string body;
};

SetLookup FindSet(db::RedisDb* redis_db, std::string_view key);
SetReply EncodeMembers(const Set& set);
using SetOperation = std::optional<SetReply> (*)(db::RedisDb*,
                                                 const CommandArgs&);

std::optional<SetReply> SInter(db::RedisDb* redis_db, const CommandArgs& keys);
std::optional<SetReply> SUnion(db::RedisDb* redis_db, const CommandArgs& keys);
std::optional<SetReply> SDiff(db::RedisDb* redis_db, const CommandArgs& keys);
void AddSetOperationReply(Client* client, SetOperation operation);

SetLookup FindSet(db::RedisDb* const redis_db, std::string_view key) {
  const auto [object, wrong_type] =
      LookupTyped(redis_db, key, db::RedisObject::ObjectType::kSet);
  if (wrong_type) {
    return {nullptr, SetLookupStatus::kWrongType};
  }
  if (object == nullptr) {
    return {nullptr, SetLookupStatus::kMissing};
  }
  return {object->Set(), SetLookupStatus::kOk};
}

SetReply EncodeMembers(const Set& set) {
  std::string body;
  set.ForEachMember([&body](std::string_view member) {
    reply::AppendBulkString(member, &body);
    return true;
  });
  return {set.Size(), std::move(body)};
}

std::optional<SetReply> SInter(db::RedisDb* const redis_db,
                               const CommandArgs& keys) {
  std::vector<const Set*> sets;
  sets.reserve(keys.size());
  bool any_missing = false;
  // Like Redis, check every key's type before a missing key empties the
  // result, so a later wrong-type key still fails the command.
  for (std::string_view key : keys) {
    const SetLookup lookup = FindSet(redis_db, key);
    if (lookup.status == SetLookupStatus::kWrongType) {
      return std::nullopt;
    }
    if (lookup.status == SetLookupStatus::kMissing) {
      any_missing = true;
      continue;
    }
    sets.push_back(lookup.set);
  }
  if (any_missing) {
    return SetReply{};
  }

  auto smallest = std::ranges::min_element(sets, {}, &Set::Size);
  std::string body;
  size_t count = 0;
  (*smallest)->ForEachMemberWithHash(
      [&sets, &body, &count, smallest](std::string_view member,
                                       const in_memory::HashedKey* hashed) {
        bool present = true;
        for (const auto* set : sets) {
          if (set != *smallest && !set->HasMember(member, hashed)) {
            present = false;
            break;
          }
        }
        if (present) {
          reply::AppendBulkString(member, &body);
          ++count;
        }
        return true;
      });
  return SetReply{count, std::move(body)};
}

std::optional<SetReply> SUnion(db::RedisDb* const redis_db,
                               const CommandArgs& keys) {
  if (keys.size() == 1) {
    const SetLookup lookup = FindSet(redis_db, keys.front());
    if (lookup.status == SetLookupStatus::kMissing) {
      return SetReply{};
    }
    if (lookup.status != SetLookupStatus::kOk) {
      return std::nullopt;
    }
    return EncodeMembers(*lookup.set);
  }

  // Dedupe through a Dict, which reuses the hashes stored by dict-encoded
  // sources instead of hashing each member again.
  using MemberDict = in_memory::Dict<std::string, std::nullptr_t>;
  std::vector<const Set*> sets;
  sets.reserve(keys.size());
  size_t total = 0;
  for (std::string_view key : keys) {
    const SetLookup lookup = FindSet(redis_db, key);
    if (lookup.status == SetLookupStatus::kMissing) {
      continue;
    }
    if (lookup.status != SetLookupStatus::kOk) {
      return std::nullopt;
    }
    sets.push_back(lookup.set);
    total += lookup.set->Size();
  }
  const auto members = MemberDict::Create(total);
  // Sizing fails only past the address space, which existing sets cannot reach.
  RS_CHECK(members != nullptr);
  for (const Set* set : sets) {
    set->ForEachMemberWithHash([&members](std::string_view member,
                                          const in_memory::HashedKey* hashed) {
      if (hashed != nullptr) {
        members->FindOrInsert(*hashed);
      } else {
        members->FindOrInsert(member);
      }
      return true;
    });
  }
  std::string body;
  auto it = MemberDict::Iterator(members.get());
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    reply::AppendBulkString(it.Key(), &body);
  }
  return SetReply{members->Size(), std::move(body)};
}

std::optional<SetReply> SDiff(db::RedisDb* const redis_db,
                              const CommandArgs& keys) {
  const SetLookup first = FindSet(redis_db, keys.front());
  if (first.status == SetLookupStatus::kWrongType) {
    return std::nullopt;
  }

  std::vector<const Set*> subtract_sets;
  subtract_sets.reserve(keys.size() - 1);
  for (auto it = keys.begin() + 1; it != keys.end(); ++it) {
    const SetLookup lookup = FindSet(redis_db, *it);
    if (lookup.status == SetLookupStatus::kMissing) {
      continue;
    }
    if (lookup.status != SetLookupStatus::kOk) {
      return std::nullopt;
    }
    subtract_sets.push_back(lookup.set);
  }
  // Every key's type is checked first, as in Redis, even when the first set
  // is missing and the difference is empty.
  if (first.status == SetLookupStatus::kMissing) {
    return SetReply{};
  }

  std::string body;
  size_t count = 0;
  first.set->ForEachMemberWithHash(
      [&subtract_sets, &body, &count](std::string_view member,
                                      const in_memory::HashedKey* hashed) {
        bool removed = false;
        for (const auto* set : subtract_sets) {
          if (set->HasMember(member, hashed)) {
            removed = true;
            break;
          }
        }
        if (!removed) {
          reply::AppendBulkString(member, &body);
          ++count;
        }
        return true;
      });
  return SetReply{count, std::move(body)};
}

void AddSetOperationReply(Client* const client, SetOperation operation) {
  auto encoded = operation(client->Db(), client->Args());
  if (!encoded.has_value()) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  client->AddReply(reply::FromSetHeader(encoded->count, client->Protocol()),
                   std::move(encoded->body));
}
}  // namespace

void HandleSInter(Client* const client) {
  AddSetOperationReply(client, SInter);
}

void HandleSUnion(Client* const client) {
  AddSetOperationReply(client, SUnion);
}

void HandleSDiff(Client* const client) { AddSetOperationReply(client, SDiff); }
}  // namespace redis_simple::command::sets
