#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/scan_options.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/reply.h"
#include "utils/strings.h"

namespace redis_simple::commands::key {
namespace {
using DeleteOperation = db::DbStatus (db::RedisDb::*)(std::string_view);

void AddDeleteReply(Client* const client, DeleteOperation operation) {
  db::RedisDb* const redis_db = client->Db();
  int64_t deleted = 0;
  for (const std::string_view key : client->Args()) {
    if ((redis_db->*operation)(key) == db::DbStatus::kOk) {
      ++deleted;
    }
  }
  if (deleted > 0) {
    client->MarkModified(deleted);
  }
  client->AddReply(reply::FromInt64(deleted));
}
}  // namespace

void HandleDel(Client* const client) {
  AddDeleteReply(client, &db::RedisDb::DeleteKey);
}

void HandleUnlink(Client* const client) {
  AddDeleteReply(client, &db::RedisDb::UnlinkKey);
}

void HandleExists(Client* const client) {
  int64_t existing = 0;
  for (const std::string_view key : client->Args()) {
    if (client->Db()->LookupKey(key) != nullptr) {
      ++existing;
    }
  }
  client->AddReply(reply::FromInt64(existing));
}

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
  kUnsupportedOption,
  kNxIncompatible,
  kGtLtIncompatible,
};

// Parses options before the time, in Redis' order, so an unsupported or
// conflicting option is reported ahead of a bad time. `*bad_option` names
// an unsupported option.
ParseExpireStatus ParseExpireArgs(const CommandArgs& args,
                                  ExpireArgs* const expire_args,
                                  std::string_view* const bad_option) {
  bool nx = false;
  bool xx = false;
  bool gt = false;
  bool lt = false;
  for (size_t i = 2; i < args.size(); ++i) {
    const std::string_view option = args[i];
    if (utils::EqualsIgnoreCase(option, "NX")) {
      nx = true;
    } else if (utils::EqualsIgnoreCase(option, "XX")) {
      xx = true;
    } else if (utils::EqualsIgnoreCase(option, "GT")) {
      gt = true;
    } else if (utils::EqualsIgnoreCase(option, "LT")) {
      lt = true;
    } else {
      *bad_option = option;
      return ParseExpireStatus::kUnsupportedOption;
    }
  }
  if (nx && (xx || gt || lt)) {
    return ParseExpireStatus::kNxIncompatible;
  }
  if (gt && lt) {
    return ParseExpireStatus::kGtLtIncompatible;
  }
  if (!utils::ToInt64(args[1], &expire_args->ttl)) {
    return ParseExpireStatus::kInvalidInteger;
  }
  expire_args->key = args[0];
  // GT already requires an existing TTL, so XX adds nothing to it.
  if (nx) {
    expire_args->condition = db::ExpireCondition::kIfNoExpiry;
  } else if (gt) {
    expire_args->condition = db::ExpireCondition::kGreaterThan;
  } else if (lt) {
    expire_args->condition = xx ? db::ExpireCondition::kIfHasExpiryAndLessThan
                                : db::ExpireCondition::kLessThan;
  } else if (xx) {
    expire_args->condition = db::ExpireCondition::kIfHasExpiry;
  }
  return ParseExpireStatus::kOk;
}

// Redis' expireGenericCommand: scales `time` to milliseconds and adds
// `base`, the current time for a relative expiration and 0 for an absolute
// one. Negative times are allowed, and expire the key at once; only a
// result that overflows is invalid, in either direction when scaling.
bool ToAbsoluteMilliseconds(int64_t time, int64_t multiplier, int64_t base,
                            int64_t* const expire) {
  if (time > std::numeric_limits<int64_t>::max() / multiplier ||
      time < std::numeric_limits<int64_t>::min() / multiplier) {
    return false;
  }
  const int64_t milliseconds = time * multiplier;
  if (milliseconds > std::numeric_limits<int64_t>::max() - base) {
    return false;
  }
  *expire = milliseconds + base;
  return true;
}

void AddExpireReply(Client* const client, int64_t multiplier, bool absolute,
                    std::string_view command) {
  ExpireArgs args;
  std::string_view bad_option;
  switch (ParseExpireArgs(client->Args(), &args, &bad_option)) {
    case ParseExpireStatus::kOk:
      break;
    case ParseExpireStatus::kInvalidInteger:
      client->AddReply(reply::kNotAnIntegerError);
      return;
    case ParseExpireStatus::kUnsupportedOption: {
      std::string message = "ERR Unsupported option ";
      message.append(bad_option);
      std::ranges::replace(message, '\r', ' ');
      std::ranges::replace(message, '\n', ' ');
      client->AddReply(reply::FromError(message));
      return;
    }
    case ParseExpireStatus::kNxIncompatible:
      client->AddReply(reply::FromError(
          "ERR NX and XX, GT or LT options at the same time are not "
          "compatible"));
      return;
    case ParseExpireStatus::kGtLtIncompatible:
      client->AddReply(reply::FromError(
          "ERR GT and LT options at the same time are not compatible"));
      return;
  }
  int64_t expire = 0;
  // An absolute time needs no clock read.
  const int64_t base = absolute ? 0 : client->Db()->Now();
  if (!ToAbsoluteMilliseconds(args.ttl, multiplier, base, &expire)) {
    std::string message = "ERR invalid expire time in '";
    message.append(command).append("' command");
    client->AddReply(reply::FromError(message));
    return;
  }
  const db::DbStatus status =
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
  AddExpireReply(client, kMillisecondsPerSecond, false, "expire");
}

void HandleExpireAt(Client* const client) {
  AddExpireReply(client, kMillisecondsPerSecond, true, "expireat");
}

void HandlePExpire(Client* const client) {
  AddExpireReply(client, 1, false, "pexpire");
}

void HandlePExpireAt(Client* const client) {
  AddExpireReply(client, 1, true, "pexpireat");
}

void HandleTtl(Client* const client) {
  AddTtlReply(client, db::TtlResolution::kSeconds);
}

void HandlePTtl(Client* const client) {
  AddTtlReply(client, db::TtlResolution::kMilliseconds);
}

void HandlePersist(Client* const client) {
  const db::DbStatus status = client->Db()->PersistKey(client->Args()[0]);
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
  // FLUSHDB [ASYNC | SYNC]. Flushing is always synchronous here, so both
  // modes behave the same.
  const commands::CommandArgs& args = client->Args();
  if (args.size() > 1 ||
      (args.size() == 1 && !utils::EqualsIgnoreCase(args[0], "ASYNC") &&
       !utils::EqualsIgnoreCase(args[0], "SYNC"))) {
    client->AddReply(reply::kSyntaxError);
    return;
  }
  db::RedisDb* const redis_db = client->Db();
  // Redis counts every key the flush removes.
  if (redis_db->KeyCount() > 0) {
    client->MarkModified(static_cast<int64_t>(redis_db->KeyCount()));
  }
  redis_db->Flush();
  client->AddReply(reply::kOk);
}

void HandleScan(Client* const client) {
  size_t cursor = 0;
  ScanOptions options;
  if (!ParseScanCursor(client, client->Args()[0], &cursor) ||
      !ParseScanOptions(client, 1, ScanTarget::kKeyspace, &options)) {
    return;
  }
  db::RedisDb* const redis_db = client->Db();
  ScanReply reply(std::min(options.count, redis_db->KeyCount()));
  // Like Redis, filter the visited keys, so a filtered step may return fewer
  // keys than COUNT, or none, with a nonzero cursor.
  const auto add_key = [&options, &reply](std::string_view key,
                                          const db::RedisObject& object) {
    if ((options.filter_type && options.type != object.Type()) ||
        !options.Matches(key)) {
      return;
    }
    reply.Add(key);
  };
  const size_t next_cursor = redis_db->ScanKeys(cursor, options.count, add_key);
  std::move(reply).Send(client, next_cursor);
}

void HandleType(Client* const client) {
  const db::RedisObject* const object =
      client->Db()->LookupKey(client->Args()[0]);
  client->AddReply(reply::FromString(
      object == nullptr ? "none" : db::TypeName(object->Type())));
}
}  // namespace redis_simple::commands::key
