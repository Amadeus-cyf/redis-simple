#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/string_utils.h"
#include "utils/time_utils.h"

namespace redis_simple::command::key {
namespace {
struct KeyArgs {
  std::string_view key;
};

struct ExpireArgs {
  std::string_view key;
  int64_t ttl{0};
  db::ExpireCondition condition{db::ExpireCondition::kNone};
};

enum class ParseExpireStatus : uint8_t {
  kOk,
  kWrongArity,
  kInvalidInteger,
  kSyntaxError,
};

struct RenameArgs {
  std::string_view old_key;
  std::string_view new_key;
};

int ParseKeyArgs(const CommandArgs& args, KeyArgs* key_args) {
  if (args.size() != 1) {
    return -1;
  }
  key_args->key = args[0];
  return 0;
}

ParseExpireStatus ParseExpireArgs(const CommandArgs& args,
                                  ExpireArgs* expire_args) {
  if (args.size() < 2) {
    return ParseExpireStatus::kWrongArity;
  }
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

int ParseRenameArgs(const CommandArgs& args, RenameArgs* rename_args) {
  if (args.size() != 2) {
    return -1;
  }
  rename_args->old_key = args[0];
  rename_args->new_key = args[1];
  return 0;
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

bool AddExpireParseError(Client* const client, ParseExpireStatus status) {
  switch (status) {
    case ParseExpireStatus::kOk:
      return false;
    case ParseExpireStatus::kWrongArity:
      client->AddReply(reply::WrongNumberOfArguments());
      return true;
    case ParseExpireStatus::kInvalidInteger:
      client->AddReply(
          reply::FromError("ERR value is not an integer or out of range"));
      return true;
    case ParseExpireStatus::kSyntaxError:
      client->AddReply(reply::SyntaxError());
      return true;
  }
  return true;
}

void AddExpireReply(Client* const client, int64_t multiplier, bool absolute) {
  ExpireArgs args;
  if (AddExpireParseError(client, ParseExpireArgs(client->Args(), &args))) {
    return;
  }
  if (auto* redis_db = client->Db()) {
    int64_t expire = 0;
    const bool valid_expire =
        absolute ? ScaleAbsoluteExpiration(args.ttl, multiplier, &expire)
                 : ExpireAtFromTtl(args.ttl, multiplier,
                                   utils::NowInMilliseconds(), &expire);
    if (!valid_expire) {
      client->AddReply(reply::FromError("ERR invalid expire time"));
      return;
    }
    const auto status = redis_db->ExpireKeyAt(args.key, expire, args.condition);
    if (status == db::DbStatus::kOk) {
      client->MarkModified();
    }
    client->AddReply(reply::FromInt64(status == db::DbStatus::kOk ? 1 : 0));
    return;
  }
  client->AddReply(reply::FromError("ERR db unavailable"));
}

void AddTtlReply(Client* const client, db::TtlResolution resolution) {
  KeyArgs args;
  if (ParseKeyArgs(client->Args(), &args) < 0) {
    client->AddReply(reply::WrongNumberOfArguments());
    return;
  }
  if (auto* redis_db = client->Db()) {
    client->AddReply(
        reply::FromInt64(redis_db->TimeToLive(args.key, resolution)));
    return;
  }
  client->AddReply(reply::FromError("ERR db unavailable"));
}
}  // namespace

void HandleExpire(Client* const client) {
  constexpr int64_t kMillisecondsPerSecond = 1000;
  AddExpireReply(client, kMillisecondsPerSecond, false);
}

void HandleExpireAt(Client* const client) {
  constexpr int64_t kMillisecondsPerSecond = 1000;
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
  KeyArgs args;
  if (ParseKeyArgs(client->Args(), &args) < 0) {
    client->AddReply(reply::WrongNumberOfArguments());
    return;
  }
  if (auto* redis_db = client->Db()) {
    const auto status = redis_db->PersistKey(args.key);
    if (status == db::DbStatus::kOk) {
      client->MarkModified();
    }
    client->AddReply(reply::FromInt64(status == db::DbStatus::kOk ? 1 : 0));
    return;
  }
  client->AddReply(reply::FromError("ERR db unavailable"));
}

void HandleRename(Client* const client) {
  RenameArgs args;
  if (ParseRenameArgs(client->Args(), &args) < 0) {
    client->AddReply(reply::WrongNumberOfArguments());
    return;
  }
  if (auto* redis_db = client->Db()) {
    if (redis_db->RenameKey(args.old_key, args.new_key) != db::DbStatus::kOk) {
      client->AddReply(reply::FromError("ERR no such key"));
      return;
    }
    if (args.old_key != args.new_key) {
      client->MarkModified();
    }
    client->AddReply(reply::FromString("OK"));
    return;
  }
  client->AddReply(reply::FromError("ERR db unavailable"));
}

void HandleDbSize(Client* const client) {
  if (!client->Args().empty()) {
    client->AddReply(reply::WrongNumberOfArguments());
    return;
  }
  if (auto* redis_db = client->Db()) {
    if (redis_db->KeyCount() >
        static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
      client->AddReply(reply::FromError("ERR db size out of range"));
      return;
    }
    client->AddReply(
        reply::FromInt64(static_cast<int64_t>(redis_db->KeyCount())));
    return;
  }
  client->AddReply(reply::FromError("ERR db unavailable"));
}

void HandleFlushDb(Client* const client) {
  if (!client->Args().empty()) {
    client->AddReply(reply::WrongNumberOfArguments());
    return;
  }
  if (auto* redis_db = client->Db()) {
    if (redis_db->KeyCount() > 0) {
      client->MarkModified();
    }
    redis_db->Flush();
    client->AddReply(reply::FromString("OK"));
    return;
  }
  client->AddReply(reply::FromError("ERR db unavailable"));
}
}  // namespace redis_simple::command::key
