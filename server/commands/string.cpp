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
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/check.h"
#include "utils/strings.h"
#include "utils/time.h"

namespace redis_simple::commands::string {
void HandleGet(Client* const client) {
  const auto [object, wrong_type] = LookupTyped(
      client->Db(), client->Args()[0], db::RedisObject::ObjectType::kString);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (object == nullptr) {
    client->AddReply(reply::NullView(client->Protocol()));
    return;
  }
  client->AddBulkReply(object->String());
}

namespace {

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
  if (seconds <= 0 || seconds > std::numeric_limits<int64_t>::max() /
                                    utils::kMillisecondsPerSecond) {
    return false;
  }
  *milliseconds = seconds * utils::kMillisecondsPerSecond;
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

// Parses the option at `*index` and advances past it. An expiration's value is
// only recorded here, so syntax errors win over value errors, as in Redis.
bool ParseSetOption(const CommandArgs& args, size_t* const index,
                    SetArgs* const set_args, ExpireUnit* const unit,
                    std::string_view* const expire_arg) {
  const std::string_view option = args[*index];
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
    ++(*index);
    return true;
  }
  if (utils::EqualsIgnoreCase(option, "GET")) {
    set_args->return_old_value = true;
    ++(*index);
    return true;
  }
  const bool keep_ttl = set_args->existing_ttl == db::ExistingTtl::kKeep;
  if (utils::EqualsIgnoreCase(option, "KEEPTTL")) {
    if (*unit != ExpireUnit::kNone) {
      return false;
    }
    set_args->existing_ttl = db::ExistingTtl::kKeep;
    ++(*index);
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
      *index + 1 >= args.size()) {
    return false;
  }
  *unit = option_unit;
  *expire_arg = args[*index + 1];
  *index += 2;
  return true;
}

// Converts the expiration option's value to an absolute time in milliseconds.
ParseStatus ResolveExpire(ExpireUnit unit, std::string_view expire_arg,
                          int64_t now, int64_t* const expire) {
  const std::optional<int64_t> expiration = utils::ToInt64(expire_arg);
  if (!expiration.has_value()) {
    return ParseStatus::kNotAnInteger;
  }
  if (*expiration <= 0) {
    return ParseStatus::kInvalidExpire;
  }
  bool valid = false;
  switch (unit) {
    case ExpireUnit::kRelativeSeconds:
    case ExpireUnit::kRelativeMilliseconds:
      valid = ExpireAtFromTtl(*expiration,
                              unit == ExpireUnit::kRelativeSeconds
                                  ? utils::kMillisecondsPerSecond
                                  : 1,
                              now, expire);
      break;
    case ExpireUnit::kAbsoluteSeconds:
      valid = MillisecondsFromSeconds(*expiration, expire);
      break;
    case ExpireUnit::kAbsoluteMilliseconds:
      *expire = *expiration;
      valid = true;
      break;
    case ExpireUnit::kNone:
      utils::Unreachable();
  }
  return valid && *expire > 0 ? ParseStatus::kOk : ParseStatus::kInvalidExpire;
}

// `db` supplies the time for relative expirations.
ParseStatus ParseArgs(const CommandArgs& args, const db::RedisDb& db,
                      SetArgs* const set_args) {
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
  return ResolveExpire(unit, expire_arg, db.Now(), &set_args->expire);
}
}  // namespace

void HandleSet(Client* const client) {
  SetArgs parsed;
  switch (ParseArgs(client->Args(), *client->Db(), &parsed)) {
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
  const bool exists = existing != nullptr;
  if (args->return_old_value && exists &&
      existing->Type() != db::RedisObject::ObjectType::kString) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const bool blocked =
      (args->condition == SetCondition::kIfMissing && exists) ||
      (args->condition == SetCondition::kIfExists && !exists);
  // GET replies with the old value whether or not the key is set. Queue it
  // before the write below overwrites that value in place, which saves
  // copying it. Without GET, a blocked SET replies with null.
  if (args->return_old_value && exists) {
    client->AddBulkReply(existing->String());
  } else if (args->return_old_value || blocked) {
    client->AddReply(reply::NullView(client->Protocol()));
  }
  if (blocked) {
    return;
  }
  redis_db->SetStringKey(args->key, args->value, args->expire,
                         args->existing_ttl);
  client->MarkModified();
  if (!args->return_old_value) {
    client->AddReply(reply::kOk);
  }
}

namespace {
constexpr auto kString = db::RedisObject::ObjectType::kString;

enum class IntegerOperation : std::uint8_t {
  kAdd,
  kSubtract,
};

bool ApplyIntegerOperation(int64_t value, int64_t operand,
                           IntegerOperation operation, int64_t* result) {
  if (operation == IntegerOperation::kAdd) {
    if ((operand > 0 &&
         value > std::numeric_limits<int64_t>::max() - operand) ||
        (operand < 0 &&
         value < std::numeric_limits<int64_t>::min() - operand)) {
      return false;
    }
    *result = value + operand;
    return true;
  }

  if ((operand > 0 && value < std::numeric_limits<int64_t>::min() + operand) ||
      (operand < 0 && value > std::numeric_limits<int64_t>::max() + operand)) {
    return false;
  }
  *result = value - operand;
  return true;
}

void UpdateInteger(Client* const client, std::string_view key, int64_t operand,
                   IntegerOperation operation) {
  const auto [object, wrong_type] =
      MutableLookupTyped(client->Db(), key, kString);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  int64_t value = 0;
  if (object != nullptr) {
    const std::optional<int64_t> current = object->Int();
    if (!current.has_value()) {
      client->AddReply(reply::kNotAnIntegerError);
      return;
    }
    value = *current;
  }
  int64_t next = 0;
  if (!ApplyIntegerOperation(value, operand, operation, &next)) {
    client->AddReply(reply::kOverflowError);
    return;
  }
  if (object != nullptr) {
    object->SetInt(next);
  } else {
    client->Db()->SetKey(key, db::RedisObject::CreateWithInt(next), 0);
  }
  client->MarkModified();
  client->AddReply(reply::FromInt64(next));
}

void HandleIncrementBy(Client* const client, IntegerOperation operation) {
  const commands::CommandArgs& args = client->Args();
  const std::optional<int64_t> operand = utils::ToInt64(args[1]);
  if (!operand.has_value()) {
    client->AddReply(reply::kNotAnIntegerError);
    return;
  }
  // Redis negates the DECRBY operand first, which INT64_MIN would overflow.
  if (operation == IntegerOperation::kSubtract &&
      *operand == std::numeric_limits<int64_t>::min()) {
    client->AddReply(reply::FromError("ERR decrement would overflow"));
    return;
  }
  UpdateInteger(client, args[0], *operand, operation);
}

int64_t ToReplyInteger(size_t value) { return static_cast<int64_t>(value); }
}  // namespace

void HandleIncr(Client* const client) {
  UpdateInteger(client, client->Args()[0], 1, IntegerOperation::kAdd);
}

void HandleDecr(Client* const client) {
  UpdateInteger(client, client->Args()[0], 1, IntegerOperation::kSubtract);
}

void HandleIncrBy(Client* const client) {
  HandleIncrementBy(client, IntegerOperation::kAdd);
}

void HandleDecrBy(Client* const client) {
  HandleIncrementBy(client, IntegerOperation::kSubtract);
}

void HandleAppend(Client* const client) {
  const commands::CommandArgs& args = client->Args();
  const std::string_view key = args[0];
  const std::string_view value_to_append = args[1];
  const auto [object, wrong_type] =
      MutableLookupTyped(client->Db(), key, kString);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  size_t length = value_to_append.size();
  if (object == nullptr) {
    client->Db()->SetKey(
        key, db::RedisObject::CreateWithStringCopy(value_to_append), 0);
  } else {
    std::string* const value = object->MutableString();
    value->append(value_to_append);
    length = value->size();
  }
  client->MarkModified();
  client->AddReply(reply::FromInt64(ToReplyInteger(length)));
}

void HandleStrLen(Client* const client) {
  const auto [object, wrong_type] =
      LookupTyped(client->Db(), client->Args()[0], kString);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const size_t length = object == nullptr ? 0 : object->String().size();
  client->AddReply(reply::FromInt64(ToReplyInteger(length)));
}

void HandleMGet(Client* const client) {
  const commands::CommandArgs& keys = client->Args();
  std::string encoded = reply::FromArrayHeader(keys.size());
  for (const auto& key : keys) {
    // MGET reports keys of other types as missing instead of failing.
    const db::RedisObject* const object =
        LookupTyped(client->Db(), key, kString).object;
    if (object != nullptr) {
      reply::AppendBulkString(object->String(), &encoded);
    } else {
      encoded.append(reply::NullView(client->Protocol()));
    }
  }
  client->AddReply(std::move(encoded));
}

void HandleMSet(Client* const client) {
  const commands::CommandArgs& args = client->Args();
  if (args.size() % 2 != 0) {
    client->AddReply(reply::WrongNumberOfArguments("mset"));
    return;
  }
  for (size_t i = 0; i < args.size(); i += 2) {
    client->Db()->SetStringKey(args[i], args[i + 1], 0);
  }
  client->MarkModified(static_cast<int64_t>(args.size() / 2));
  client->AddReply(reply::kOk);
}
}  // namespace redis_simple::commands::string
