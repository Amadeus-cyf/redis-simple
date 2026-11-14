#include "server/aof/records.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "server/db/db.h"
#include "server/reply.h"

namespace redis_simple::aof {
namespace {
// The records below are built with reply::AppendBulkString, which grows
// `output` geometrically, so a batch of records costs a few allocations
// however many commands it holds.

void AppendCommand(std::string_view command,
                   const std::vector<std::string_view>& args,
                   std::string* const output) {
  reply::AppendArrayHeader(args.size() + 1, output);
  reply::AppendBulkString(command, output);
  for (const std::string_view argument : args) {
    reply::AppendBulkString(argument, output);
  }
}

// How many decimal digits `value` takes.
size_t DecimalDigits(size_t value) {
  size_t digits = 1;
  for (; value >= 10; value /= 10) {
    ++digits;
  }
  return digits;
}
}  // namespace

int64_t UnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void AppendTimestamp(int64_t seconds, std::string* const output) {
  output->append("#TS:").append(std::to_string(seconds)).append("\r\n");
}

bool EncodeMutation(std::string_view command,
                    const std::vector<std::string_view>& args,
                    db::RedisDb* const db, std::string* const output) {
  if (command == "SET") {
    if (args.size() < 2) {
      return false;
    }
    const std::string_view key = args[0];
    if (args.size() == 2) {
      // No options: the key now holds exactly this value and no TTL, so the
      // command is its own record and the stored value need not be read.
      AppendCommand(command, args, output);
      return true;
    }
    // Options (NX, XX, GET, KEEPTTL, a TTL) decide what was stored, so
    // record the result.
    const auto [object, expire] = db->LookupKeyWithExpiration(key);
    if (object == nullptr) {
      AppendFixedCommand({"DEL", key}, output);
      return true;
    }
    if (object->Type() != db::RedisObject::ObjectType::kString) {
      return false;
    }
    AppendFixedCommand({"SET", key, object->String()}, output);
    if (expire.has_value()) {
      AppendExpireAt(key, *expire, output);
    }
    return true;
  }
  if (command == "EXPIRE" || command == "EXPIREAT" || command == "PEXPIRE" ||
      command == "PEXPIREAT") {
    if (args.empty()) {
      return false;
    }
    const std::string_view key = args[0];
    if (const std::optional<int64_t> expire = db->Expiration(key)) {
      AppendExpireAt(key, *expire, output);
    } else {
      AppendFixedCommand({"DEL", key}, output);
    }
    return true;
  }
  AppendCommand(command, args, output);
  return true;
}

void AppendFixedCommand(std::initializer_list<std::string_view> elements,
                        std::string* const output) {
  reply::AppendArrayHeader(elements.size(), output);
  for (const std::string_view element : elements) {
    reply::AppendBulkString(element, output);
  }
}

void AppendExpireAt(std::string_view key, int64_t expire,
                    std::string* const output) {
  AppendFixedCommand({"PEXPIREAT", key, std::to_string(expire)}, output);
}

size_t RespBulkSize(std::string_view value) {
  // "$", CRLF after the length, and CRLF after the value.
  constexpr size_t kFramingBytes = 5;
  const size_t digits = DecimalDigits(value.size());
  if (value.size() >
      std::numeric_limits<size_t>::max() - digits - kFramingBytes) {
    return std::numeric_limits<size_t>::max();
  }
  return value.size() + digits + kFramingBytes;
}

size_t RespArrayHeaderSize(size_t element_count) {
  // "*" and CRLF.
  constexpr size_t kFramingBytes = 3;
  return DecimalDigits(element_count) + kFramingBytes;
}

bool AddSize(size_t value, size_t* const total) {
  if (value > std::numeric_limits<size_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}
}  // namespace redis_simple::aof
