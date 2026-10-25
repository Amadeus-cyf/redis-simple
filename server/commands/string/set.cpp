#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/string_utils.h"
#include "utils/time_utils.h"

namespace redis_simple::command::strings {
namespace {
constexpr int64_t kMillisecondsPerSecond = 1000;

enum class SetCondition : uint8_t {
  kNone,
  kIfMissing,
  kIfExists,
};

struct SetArgs {
  std::string_view key;
  std::string_view value;
  int64_t expire{0};
  int flags{0};
  SetCondition condition{SetCondition::kNone};
  bool return_old_value{};
};

bool ExpireAtFromTtl(int64_t ttl, int64_t multiplier, int64_t now,
                     int64_t* expire) {
  if (ttl <= 0 || ttl > std::numeric_limits<int64_t>::max() / multiplier) {
    return false;
  }
  const int64_t ttl_ms = ttl * multiplier;
  if (ttl_ms > std::numeric_limits<int64_t>::max() - now) {
    return false;
  }
  *expire = now + ttl_ms;
  return true;
}

bool MillisecondsFromSeconds(int64_t seconds, int64_t* milliseconds) {
  if (seconds <= 0 ||
      seconds > std::numeric_limits<int64_t>::max() / kMillisecondsPerSecond) {
    return false;
  }
  *milliseconds = seconds * kMillisecondsPerSecond;
  return true;
}

// Parses the option at `*idx` and advances past it.
bool ParseSetOption(const CommandArgs& args, size_t* const idx,
                    SetArgs* const set_args) {
  const std::string_view option = args[*idx];
  const bool if_missing = utils::EqualsIgnoreCase(option, "NX");
  const bool if_exists = utils::EqualsIgnoreCase(option, "XX");
  if (if_missing || if_exists) {
    if (set_args->condition != SetCondition::kNone) {
      return false;
    }
    set_args->condition =
        if_missing ? SetCondition::kIfMissing : SetCondition::kIfExists;
    ++(*idx);
    return true;
  }
  if (utils::EqualsIgnoreCase(option, "GET")) {
    if (set_args->return_old_value) {
      return false;
    }
    set_args->return_old_value = true;
    ++(*idx);
    return true;
  }
  const bool keep_ttl = db::HasFlag(set_args->flags, db::SetKeyFlag::kKeepTtl);
  if (utils::EqualsIgnoreCase(option, "KEEPTTL")) {
    if (set_args->expire > 0 || keep_ttl) {
      return false;
    }
    set_args->flags |= db::ToInt(db::SetKeyFlag::kKeepTtl);
    ++(*idx);
    return true;
  }
  const bool relative_seconds = utils::EqualsIgnoreCase(option, "EX");
  const bool relative_milliseconds = utils::EqualsIgnoreCase(option, "PX");
  const bool absolute_seconds = utils::EqualsIgnoreCase(option, "EXAT");
  const bool absolute_milliseconds = utils::EqualsIgnoreCase(option, "PXAT");
  if (!relative_seconds && !relative_milliseconds && !absolute_seconds &&
      !absolute_milliseconds) {
    return false;
  }
  if (keep_ttl || set_args->expire > 0 || *idx + 1 >= args.size()) {
    return false;
  }
  int64_t expiration = 0;
  if (!utils::ToInt64(args[*idx + 1], &expiration) || expiration <= 0) {
    return false;
  }
  if (relative_seconds || relative_milliseconds) {
    const int64_t multiplier = relative_seconds ? kMillisecondsPerSecond : 1;
    if (!ExpireAtFromTtl(expiration, multiplier, utils::NowInMilliseconds(),
                         &set_args->expire)) {
      return false;
    }
  } else if (absolute_seconds) {
    if (!MillisecondsFromSeconds(expiration, &set_args->expire)) {
      return false;
    }
  } else {
    set_args->expire = expiration;
  }
  if (set_args->expire <= 0) {
    return false;
  }
  *idx += 2;
  return true;
}

std::optional<SetArgs> ParseArgs(const CommandArgs& args) {
  SetArgs set_args;
  set_args.key = args[0];
  set_args.value = args[1];
  for (size_t i = 2; i < args.size();) {
    if (!ParseSetOption(args, &i, &set_args)) {
      return std::nullopt;
    }
  }
  return set_args;
}
}  // namespace

void HandleSet(Client* const client) {
  const auto args = ParseArgs(client->Args());
  if (!args.has_value()) {
    client->AddReply(reply::kSyntaxError);
    return;
  }
  db::RedisDb* const redis_db = client->Db();
  const db::RedisObject* const existing = redis_db->LookupKey(args->key);
  std::optional<std::string> old_value;
  if (args->return_old_value && existing != nullptr) {
    if (existing->Type() != db::RedisObject::ObjectType::kString) {
      client->AddReply(reply::kWrongTypeError);
      return;
    }
    old_value = existing->String();
  }

  const bool exists = existing != nullptr;
  const bool blocked =
      (args->condition == SetCondition::kIfMissing && exists) ||
      (args->condition == SetCondition::kIfExists && !exists);
  if (!blocked) {
    auto value = db::RedisObject::CreateWithString(std::string(args->value));
    redis_db->SetKey(args->key, std::move(value), args->expire, args->flags);
    client->MarkModified();
  }
  if (!blocked && !args->return_old_value) {
    client->AddReply(reply::kOk);
  } else if (old_value.has_value()) {
    client->AddBulkReply(*old_value);
  } else {
    client->AddReply(reply::NullView(client->Protocol()));
  }
}
}  // namespace redis_simple::command::strings
