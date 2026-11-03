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
#include "utils/check.h"
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
  db::ExistingTtl existing_ttl{db::ExistingTtl::kClear};
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

enum class ExpireUnit : uint8_t {
  kNone,
  kRelativeSeconds,
  kRelativeMilliseconds,
  kAbsoluteSeconds,
  kAbsoluteMilliseconds,
};

enum class ParseStatus : uint8_t {
  kOk,
  kSyntaxError,
  kNotAnInteger,
  kInvalidExpire,
};

// Parses the option at `*idx` and advances past it. An expiration's value is
// only recorded here, so syntax errors win over value errors, as in Redis.
bool ParseSetOption(const CommandArgs& args, size_t* const idx,
                    SetArgs* const set_args, ExpireUnit* const unit,
                    std::string_view* const expire_arg) {
  const std::string_view option = args[*idx];
  const bool if_missing = utils::EqualsIgnoreCase(option, "NX");
  const bool if_exists = utils::EqualsIgnoreCase(option, "XX");
  // As in Redis, any option may repeat; only conflicting options are errors.
  if (if_missing || if_exists) {
    const SetCondition condition =
        if_missing ? SetCondition::kIfMissing : SetCondition::kIfExists;
    if (set_args->condition != SetCondition::kNone &&
        set_args->condition != condition) {
      return false;
    }
    set_args->condition = condition;
    ++(*idx);
    return true;
  }
  if (utils::EqualsIgnoreCase(option, "GET")) {
    set_args->return_old_value = true;
    ++(*idx);
    return true;
  }
  const bool keep_ttl = set_args->existing_ttl == db::ExistingTtl::kKeep;
  if (utils::EqualsIgnoreCase(option, "KEEPTTL")) {
    if (*unit != ExpireUnit::kNone) {
      return false;
    }
    set_args->existing_ttl = db::ExistingTtl::kKeep;
    ++(*idx);
    return true;
  }
  ExpireUnit option_unit = ExpireUnit::kNone;
  if (utils::EqualsIgnoreCase(option, "EX")) {
    option_unit = ExpireUnit::kRelativeSeconds;
  } else if (utils::EqualsIgnoreCase(option, "PX")) {
    option_unit = ExpireUnit::kRelativeMilliseconds;
  } else if (utils::EqualsIgnoreCase(option, "EXAT")) {
    option_unit = ExpireUnit::kAbsoluteSeconds;
  } else if (utils::EqualsIgnoreCase(option, "PXAT")) {
    option_unit = ExpireUnit::kAbsoluteMilliseconds;
  } else {
    return false;
  }
  // Repeating the same expiration option is allowed, and the last one wins.
  if (keep_ttl || (*unit != ExpireUnit::kNone && *unit != option_unit) ||
      *idx + 1 >= args.size()) {
    return false;
  }
  *unit = option_unit;
  *expire_arg = args[*idx + 1];
  *idx += 2;
  return true;
}

// Converts the expiration option's value to an absolute time in milliseconds.
ParseStatus ResolveExpire(ExpireUnit unit, std::string_view expire_arg,
                          int64_t* const expire) {
  int64_t expiration = 0;
  if (!utils::ToInt64(expire_arg, &expiration)) {
    return ParseStatus::kNotAnInteger;
  }
  if (expiration <= 0) {
    return ParseStatus::kInvalidExpire;
  }
  bool valid = false;
  switch (unit) {
    case ExpireUnit::kRelativeSeconds:
    case ExpireUnit::kRelativeMilliseconds:
      valid = ExpireAtFromTtl(
          expiration,
          unit == ExpireUnit::kRelativeSeconds ? kMillisecondsPerSecond : 1,
          utils::NowInMilliseconds(), expire);
      break;
    case ExpireUnit::kAbsoluteSeconds:
      valid = MillisecondsFromSeconds(expiration, expire);
      break;
    case ExpireUnit::kAbsoluteMilliseconds:
      *expire = expiration;
      valid = true;
      break;
    case ExpireUnit::kNone:
      utils::Unreachable();
  }
  return valid && *expire > 0 ? ParseStatus::kOk : ParseStatus::kInvalidExpire;
}

ParseStatus ParseArgs(const CommandArgs& args, SetArgs* const set_args) {
  set_args->key = args[0];
  set_args->value = args[1];
  ExpireUnit unit = ExpireUnit::kNone;
  std::string_view expire_arg;
  for (size_t i = 2; i < args.size();) {
    if (!ParseSetOption(args, &i, set_args, &unit, &expire_arg)) {
      return ParseStatus::kSyntaxError;
    }
  }
  if (unit == ExpireUnit::kNone) {
    return ParseStatus::kOk;
  }
  return ResolveExpire(unit, expire_arg, &set_args->expire);
}
}  // namespace

void HandleSet(Client* const client) {
  SetArgs parsed;
  switch (ParseArgs(client->Args(), &parsed)) {
    case ParseStatus::kOk:
      break;
    case ParseStatus::kSyntaxError:
      client->AddReply(reply::kSyntaxError);
      return;
    case ParseStatus::kNotAnInteger:
      client->AddReply(reply::kNotAnIntegerError);
      return;
    case ParseStatus::kInvalidExpire:
      client->AddReply(
          reply::FromError("ERR invalid expire time in 'set' command"));
      return;
  }
  const SetArgs* const args = &parsed;
  db::RedisDb* const redis_db = client->Db();
  const db::RedisObject* const existing = redis_db->LookupKey(args->key);
  std::optional<std::string> old_value;
  if (args->return_old_value && existing != nullptr) {
    if (existing->Type() != db::RedisObject::ObjectType::kString) {
      client->AddReply(reply::kWrongTypeError);
      return;
    }
    old_value = std::string(existing->String().View());
  }

  const bool exists = existing != nullptr;
  const bool blocked =
      (args->condition == SetCondition::kIfMissing && exists) ||
      (args->condition == SetCondition::kIfExists && !exists);
  if (!blocked) {
    redis_db->SetStringKey(args->key, args->value, args->expire,
                           args->existing_ttl);
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
