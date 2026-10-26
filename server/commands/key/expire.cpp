#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/string_utils.h"
#include "utils/time_utils.h"

namespace redis_simple::command::key {
namespace {
constexpr int64_t kMillisecondsPerSecond = 1000;

struct ExpireArgs {
  std::string_view key;
  int64_t ttl{0};
  db::ExpireCondition condition{db::ExpireCondition::kNone};
};

enum class ParseExpireStatus : uint8_t {
  kOk,
  kInvalidInteger,
  kSyntaxError,
};

ParseExpireStatus ParseExpireArgs(const CommandArgs& args,
                                  ExpireArgs* expire_args) {
  if (!utils::ToInt64(args[1], &expire_args->ttl)) {
    return ParseExpireStatus::kInvalidInteger;
  }
  expire_args->key = args[0];
  if (args.size() == 2) {
    return ParseExpireStatus::kOk;
  }
  if (args.size() != 3) {
    return ParseExpireStatus::kSyntaxError;
  }

  const std::string_view option = args[2];
  if (utils::EqualsIgnoreCase(option, "NX")) {
    expire_args->condition = db::ExpireCondition::kIfNoExpiry;
  } else if (utils::EqualsIgnoreCase(option, "XX")) {
    expire_args->condition = db::ExpireCondition::kIfHasExpiry;
  } else if (utils::EqualsIgnoreCase(option, "GT")) {
    expire_args->condition = db::ExpireCondition::kGreaterThan;
  } else if (utils::EqualsIgnoreCase(option, "LT")) {
    expire_args->condition = db::ExpireCondition::kLessThan;
  } else {
    return ParseExpireStatus::kSyntaxError;
  }
  return ParseExpireStatus::kOk;
}

bool ExpireAtFromTtl(int64_t ttl, int64_t multiplier, int64_t now,
                     int64_t* expire) {
  if (ttl <= 0) {
    *expire = now - 1;
    return true;
  }
  if (ttl > std::numeric_limits<int64_t>::max() / multiplier) {
    return false;
  }
  const int64_t ttl_ms = ttl * multiplier;
  if (ttl_ms > std::numeric_limits<int64_t>::max() - now) {
    return false;
  }
  *expire = now + ttl_ms;
  return true;
}

bool ScaleAbsoluteExpiration(int64_t expiration, int64_t multiplier,
                             int64_t* scaled_expiration) {
  if (expiration > std::numeric_limits<int64_t>::max() / multiplier ||
      expiration < std::numeric_limits<int64_t>::min() / multiplier) {
    return false;
  }
  *scaled_expiration = expiration * multiplier;
  return true;
}

void AddExpireReply(Client* const client, int64_t multiplier, bool absolute) {
  ExpireArgs args;
  switch (ParseExpireArgs(client->Args(), &args)) {
    case ParseExpireStatus::kOk:
      break;
    case ParseExpireStatus::kInvalidInteger:
      client->AddReply(
          reply::FromError("ERR value is not an integer or out of range"));
      return;
    case ParseExpireStatus::kSyntaxError:
      client->AddReply(reply::kSyntaxError);
      return;
  }
  int64_t expire = 0;
  const bool valid_expire =
      absolute ? ScaleAbsoluteExpiration(args.ttl, multiplier, &expire)
               : ExpireAtFromTtl(args.ttl, multiplier,
                                 utils::NowInMilliseconds(), &expire);
  if (!valid_expire) {
    client->AddReply(reply::FromError("ERR invalid expire time"));
    return;
  }
  const auto status =
      client->Db()->ExpireKeyAt(args.key, expire, args.condition);
  if (status == db::DbStatus::kOk) {
    client->MarkModified();
  }
  client->AddReply(reply::FromInt64(status == db::DbStatus::kOk ? 1 : 0));
}

void AddTtlReply(Client* const client, db::TtlResolution resolution) {
  client->AddReply(reply::FromInt64(
      client->Db()->TimeToLive(client->Args()[0], resolution)));
}
}  // namespace

void HandleExpire(Client* const client) {
  AddExpireReply(client, kMillisecondsPerSecond, false);
}

void HandleExpireAt(Client* const client) {
  AddExpireReply(client, kMillisecondsPerSecond, true);
}

void HandlePExpire(Client* const client) { AddExpireReply(client, 1, false); }

void HandlePExpireAt(Client* const client) { AddExpireReply(client, 1, true); }

void HandleTtl(Client* const client) {
  AddTtlReply(client, db::TtlResolution::kSeconds);
}

void HandlePTtl(Client* const client) {
  AddTtlReply(client, db::TtlResolution::kMilliseconds);
}

void HandlePersist(Client* const client) {
  const auto status = client->Db()->PersistKey(client->Args()[0]);
  if (status == db::DbStatus::kOk) {
    client->MarkModified();
  }
  client->AddReply(reply::FromInt64(status == db::DbStatus::kOk ? 1 : 0));
}

void HandleRename(Client* const client) {
  const std::string_view old_key = client->Args()[0];
  const std::string_view new_key = client->Args()[1];
  if (client->Db()->RenameKey(old_key, new_key) != db::DbStatus::kOk) {
    client->AddReply(reply::FromError("ERR no such key"));
    return;
  }
  if (old_key != new_key) {
    client->MarkModified();
  }
  client->AddReply(reply::kOk);
}

void HandleDbSize(Client* const client) {
  client->AddReply(
      reply::FromInt64(static_cast<int64_t>(client->Db()->KeyCount())));
}

void HandleFlushDb(Client* const client) {
  db::RedisDb* const redis_db = client->Db();
  if (redis_db->KeyCount() > 0) {
    client->MarkModified();
  }
  redis_db->Flush();
  client->AddReply(reply::kOk);
}
}  // namespace redis_simple::command::key
