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

namespace redis_simple::command::strings {
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
  if (object != nullptr && !utils::ToInt64(object->String(), &value)) {
    client->AddReply(reply::FromError("ERR value is not an integer"));
    return;
  }
  int64_t next = 0;
  if (!ApplyIntegerOperation(value, operand, operation, &next)) {
    client->AddReply(
        reply::FromError("ERR increment or decrement would overflow"));
    return;
  }
  std::string next_value = std::to_string(next);
  if (object != nullptr) {
    *object->MutableString() = std::move(next_value);
  } else {
    client->Db()->SetKey(
        key, db::RedisObject::CreateWithString(std::move(next_value)), 0);
  }
  client->MarkModified();
  client->AddReply(reply::FromInt64(next));
}

void HandleIncrementBy(Client* const client, IntegerOperation operation) {
  const auto& args = client->Args();
  int64_t operand = 0;
  if (!utils::ToInt64(args[1], &operand)) {
    client->AddReply(reply::FromError("ERR value is not an integer"));
    return;
  }
  UpdateInteger(client, args[0], operand, operation);
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
  const auto& args = client->Args();
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
        key, db::RedisObject::CreateWithString(std::string(value_to_append)),
        0);
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
  const auto& keys = client->Args();
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
  const auto& args = client->Args();
  if (args.size() % 2 != 0) {
    client->AddReply(reply::kWrongNumberOfArguments);
    return;
  }
  for (size_t i = 0; i < args.size(); i += 2) {
    client->Db()->SetKey(
        args[i], db::RedisObject::CreateWithString(std::string(args[i + 1])),
        0);
  }
  client->MarkModified();
  client->AddReply(reply::kOk);
}
}  // namespace redis_simple::command::strings
