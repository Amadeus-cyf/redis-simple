#include "server/aof/records.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "server/db/db.h"
#include "server/reply.h"

namespace redis_simple::aof {
namespace {
// Covers a bulk-string frame or array header, including a size_t decimal value.
constexpr size_t kRespElementOverhead = 32;
constexpr std::string_view kLongestInt64 = "-9223372036854775808";

// Reserves room in `output` for `command_count` commands with
// `element_count` elements of `payload_size` bytes in all, growing it
// geometrically. False if the size overflows.
bool ReserveResp(size_t payload_size, size_t element_count,
                 size_t command_count, std::string* const output) {
  const size_t max_size = std::numeric_limits<size_t>::max();
  if (command_count > max_size - element_count) {
    return false;
  }
  const size_t overhead_count = element_count + command_count;
  if (overhead_count > max_size / kRespElementOverhead) {
    return false;
  }
  const size_t overhead = kRespElementOverhead * overhead_count;
  size_t reserve_size = output->size();
  if (!AddSize(payload_size, &reserve_size) ||
      !AddSize(overhead, &reserve_size) || reserve_size > output->max_size()) {
    return false;
  }
  if (reserve_size > output->capacity()) {
    const size_t capacity = output->capacity();
    const size_t grown_capacity =
        capacity <= output->max_size() / 2 ? capacity * 2 : output->max_size();
    output->reserve(std::max(reserve_size, grown_capacity));
  }
  return true;
}

bool ReserveCommands(std::initializer_list<std::string_view> elements,
                     size_t command_count, std::string* const output) {
  size_t payload_size = 0;
  for (const auto element : elements) {
    if (!AddSize(element.size(), &payload_size)) {
      return false;
    }
  }
  return ReserveResp(payload_size, elements.size(), command_count, output);
}

bool ReserveCommand(std::string_view command,
                    const std::vector<std::string_view>& args,
                    std::string* const output) {
  size_t payload_size = command.size();
  for (const auto argument : args) {
    if (!AddSize(argument.size(), &payload_size)) {
      return false;
    }
  }

  if (args.size() == std::numeric_limits<size_t>::max()) {
    return false;
  }
  return ReserveResp(payload_size, args.size() + 1, 1, output);
}

bool ReserveSetRecord(std::string_view key, std::string_view value,
                      bool has_expiration, std::string* const output) {
  if (!has_expiration) {
    return ReserveCommands({"SET", key, value}, 1, output);
  }
  return ReserveCommands({"SET", key, value, "PEXPIREAT", key, kLongestInt64},
                         2, output);
}

bool AppendCommand(std::string_view command,
                   const std::vector<std::string_view>& args,
                   std::string* const output) {
  if (!ReserveCommand(command, args, output)) {
    return false;
  }
  reply::AppendArrayHeader(args.size() + 1, output);
  reply::AppendBulkString(command, output);
  for (std::string_view argument : args) {
    reply::AppendBulkString(argument, output);
  }
  return true;
}

bool AppendSingleKeyCommand(std::string_view command, std::string_view key,
                            std::string* const output) {
  if (!ReserveCommands({command, key}, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(2, output);
  reply::AppendBulkString(command, output);
  reply::AppendBulkString(key, output);
  return true;
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
  if (db == nullptr || output == nullptr) {
    return false;
  }
  if (command == "SET") {
    if (args.empty()) {
      return false;
    }
    const auto [object, expire] = db->LookupKeyWithExpiration(args[0]);
    if (object == nullptr) {
      return AppendSingleKeyCommand("DEL", args[0], output);
    }
    if (object->Type() != db::RedisObject::ObjectType::kString) {
      return false;
    }
    if (!ReserveSetRecord(args[0], object->String(), expire.has_value(),
                          output)) {
      return false;
    }
    reply::AppendArrayHeader(3, output);
    reply::AppendBulkString("SET", output);
    reply::AppendBulkString(args[0], output);
    reply::AppendBulkString(object->String(), output);
    return !expire.has_value() || AppendExpireAt(args[0], *expire, output);
  }
  if (command == "EXPIRE" || command == "EXPIREAT" || command == "PEXPIRE" ||
      command == "PEXPIREAT") {
    if (args.empty()) {
      return false;
    }
    const std::optional<int64_t> expire = db->Expiration(args[0]);
    if (expire.has_value()) {
      return AppendExpireAt(args[0], *expire, output);
    }
    return AppendSingleKeyCommand("DEL", args[0], output);
  }
  return AppendCommand(command, args, output);
}

bool AppendFixedCommand(std::initializer_list<std::string_view> elements,
                        std::string* const output) {
  if (elements.size() == 0 || !ReserveCommands(elements, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(elements.size(), output);
  for (const auto element : elements) {
    reply::AppendBulkString(element, output);
  }
  return true;
}

bool AppendExpireAt(std::string_view key, int64_t expire, std::string* output) {
  std::array<char, std::numeric_limits<int64_t>::digits10 + 3> buffer{};
  const auto encoded =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), expire);
  if (encoded.ec != std::errc()) {
    return false;
  }
  const std::string_view encoded_expire(
      buffer.data(), static_cast<size_t>(encoded.ptr - buffer.data()));
  if (!ReserveCommands({"PEXPIREAT", key, encoded_expire}, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(3, output);
  reply::AppendBulkString("PEXPIREAT", output);
  reply::AppendBulkString(key, output);
  reply::AppendBulkString(encoded_expire, output);
  return true;
}

size_t RespBulkSize(std::string_view value) {
  constexpr size_t kFramingBytes = 5;
  size_t digits = 1;
  for (size_t size = value.size(); size >= 10; size /= 10) {
    ++digits;
  }
  if (value.size() >
      std::numeric_limits<size_t>::max() - digits - kFramingBytes) {
    return std::numeric_limits<size_t>::max();
  }
  return value.size() + digits + kFramingBytes;
}

size_t RespArrayHeaderSize(size_t element_count) {
  constexpr size_t kFramingBytes = 3;
  size_t digits = 1;
  for (size_t size = element_count; size >= 10; size /= 10) {
    ++digits;
  }
  return digits + kFramingBytes;
}

bool AddSize(size_t value, size_t* const total) {
  if (value > std::numeric_limits<size_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}
}  // namespace redis_simple::aof
