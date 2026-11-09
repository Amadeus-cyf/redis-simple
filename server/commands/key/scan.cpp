#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple::commands::keys {
namespace {
constexpr size_t kDefaultScanCount = 10;
constexpr size_t kMaxInitialKeyCapacity = 1024;

enum class ParseStatus : uint8_t {
  kOk,
  kInvalidCursor,
  kInvalidCount,
  kSyntaxError,
  kNoValues,
};

struct ScanArgs {
  size_t cursor{};
  size_t count{kDefaultScanCount};
  std::optional<std::string_view> pattern;
  // Set when TYPE was given. A name that is not a type matches no key, as in
  // Redis 8, which has not made it an error yet.
  bool filter_type{};
  std::optional<db::RedisObject::ObjectType> type;
};

// Redis' scanGenericCommand for SCAN: options in any order, each read only
// when its value follows it.
ParseStatus ParseScanArgs(const CommandArgs& args, ScanArgs* const scan_args) {
  uint64_t cursor = 0;
  if (!utils::ToUint64(args[0], &cursor) ||
      std::cmp_greater(cursor, std::numeric_limits<size_t>::max())) {
    return ParseStatus::kInvalidCursor;
  }
  scan_args->cursor = static_cast<size_t>(cursor);

  size_t index = 1;
  while (index < args.size()) {
    const bool has_value = index + 1 < args.size();
    const std::string_view option = args[index];
    if (has_value && utils::EqualsIgnoreCase(option, "COUNT")) {
      int64_t count = 0;
      if (!utils::ToInt64(args[index + 1], &count)) {
        return ParseStatus::kInvalidCount;
      }
      // Redis reports a count below 1 as a syntax error.
      if (count < 1) {
        return ParseStatus::kSyntaxError;
      }
      scan_args->count = static_cast<size_t>(count);
      index += 2;
    } else if (has_value && utils::EqualsIgnoreCase(option, "MATCH")) {
      scan_args->pattern = args[index + 1];
      index += 2;
    } else if (has_value && utils::EqualsIgnoreCase(option, "TYPE")) {
      scan_args->filter_type = true;
      scan_args->type = db::TypeFromName(args[index + 1]);
      index += 2;
    } else if (utils::EqualsIgnoreCase(option, "NOVALUES")) {
      return ParseStatus::kNoValues;
    } else {
      return ParseStatus::kSyntaxError;
    }
  }
  return ParseStatus::kOk;
}

void AddParseError(Client* const client, ParseStatus status) {
  switch (status) {
    case ParseStatus::kInvalidCursor:
      client->AddReply(reply::FromError("ERR invalid cursor"));
      return;
    case ParseStatus::kInvalidCount:
      client->AddReply(reply::kNotAnIntegerError);
      return;
    case ParseStatus::kSyntaxError:
      client->AddReply(reply::kSyntaxError);
      return;
    case ParseStatus::kNoValues:
      client->AddReply(
          reply::FromError("ERR NOVALUES option can only be used in HSCAN"));
      return;
    case ParseStatus::kOk:
      return;
  }
}

size_t ScanReplyCapacity(const std::vector<std::string_view>& keys) {
  constexpr size_t kReplyOverhead = 64;
  constexpr size_t kBulkStringOverhead = 32;
  constexpr size_t kMaxSize = std::numeric_limits<size_t>::max();
  size_t capacity = kReplyOverhead;
  for (const auto key : keys) {
    if (capacity > kMaxSize - kBulkStringOverhead ||
        key.size() > kMaxSize - capacity - kBulkStringOverhead) {
      return 0;
    }
    capacity += key.size() + kBulkStringOverhead;
  }
  return capacity;
}

std::string EncodeScanReply(size_t cursor,
                            const std::vector<std::string_view>& keys) {
  std::string encoded;
  const size_t capacity = ScanReplyCapacity(keys);
  if (capacity > 0) {
    encoded.reserve(capacity);
  }

  reply::AppendArrayHeader(2, &encoded);
  std::array<char, 21> cursor_buffer{};
  const int cursor_length =
      utils::Uint64ToString(cursor_buffer, static_cast<uint64_t>(cursor));
  const std::string_view cursor_text(cursor_buffer.data(),
                                     static_cast<size_t>(cursor_length));
  reply::AppendBulkString(cursor_text, &encoded);
  reply::AppendArrayHeader(keys.size(), &encoded);
  for (const auto key : keys) {
    reply::AppendBulkString(key, &encoded);
  }
  return encoded;
}
}  // namespace

void HandleScan(Client* const client) {
  ScanArgs args;
  const ParseStatus parse_status = ParseScanArgs(client->Args(), &args);
  if (parse_status != ParseStatus::kOk) {
    AddParseError(client, parse_status);
    return;
  }

  db::RedisDb* const redis_db = client->Db();

  std::vector<std::string_view> keys;
  keys.reserve(
      std::min({args.count, redis_db->KeyCount(), kMaxInitialKeyCapacity}));
  // Like Redis, filter the sampled keys, so a filtered step may return
  // fewer keys than COUNT, or none, with a nonzero cursor.
  const auto collect_key = [&args, &keys](std::string_view key,
                                          const db::RedisObject& object) {
    if (args.filter_type && args.type != object.Type()) {
      return;
    }
    if (args.pattern.has_value() && !utils::MatchesGlob(key, *args.pattern)) {
      return;
    }
    keys.push_back(key);
  };
  const size_t next_cursor =
      redis_db->ScanKeys(args.cursor, args.count, collect_key);
  client->AddReply(EncodeScanReply(next_cursor, keys));
}
}  // namespace redis_simple::commands::keys
