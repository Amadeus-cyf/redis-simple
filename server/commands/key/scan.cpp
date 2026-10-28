#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple::command::key {
namespace {
constexpr size_t kDefaultScanCount = 10;
constexpr size_t kMaxInitialKeyCapacity = 1024;

enum class ParseStatus : uint8_t {
  kOk,
  kInvalidCursor,
  kInvalidCount,
  kSyntaxError,
};

struct ScanArgs {
  size_t cursor{};
  size_t count{kDefaultScanCount};
  std::optional<std::string_view> pattern;
};

bool FitsInSizeT(int64_t value) {
  return value >= 0 &&
         static_cast<uint64_t>(value) <=
             static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

ParseStatus ParseScanArgs(const CommandArgs& args, ScanArgs* scan_args) {
  int64_t cursor = 0;
  if (!utils::ToInt64(args[0], &cursor) || !FitsInSizeT(cursor)) {
    return ParseStatus::kInvalidCursor;
  }
  scan_args->cursor = static_cast<size_t>(cursor);

  for (size_t index = 1; index < args.size(); index += 2) {
    if (index + 1 >= args.size()) {
      return ParseStatus::kSyntaxError;
    }
    if (utils::EqualsIgnoreCase(args[index], "MATCH")) {
      scan_args->pattern = args[index + 1];
      continue;
    }
    if (utils::EqualsIgnoreCase(args[index], "COUNT")) {
      int64_t count = 0;
      if (!utils::ToInt64(args[index + 1], &count) || count <= 0 ||
          !FitsInSizeT(count)) {
        return ParseStatus::kInvalidCount;
      }
      scan_args->count = static_cast<size_t>(count);
      continue;
    }
    return ParseStatus::kSyntaxError;
  }
  return ParseStatus::kOk;
}

void AddParseError(Client* const client, ParseStatus status) {
  switch (status) {
    case ParseStatus::kInvalidCursor:
      client->AddReply(reply::FromError("ERR invalid cursor"));
      return;
    case ParseStatus::kInvalidCount:
      client->AddReply(
          reply::FromError("ERR value is not an integer or out of range"));
      return;
    case ParseStatus::kSyntaxError:
      client->AddReply(reply::kSyntaxError);
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
  const auto collect_key = [&args, &keys](std::string_view key) {
    const bool matches_pattern =
        !args.pattern.has_value() || utils::MatchesGlob(key, *args.pattern);
    if (matches_pattern) {
      keys.push_back(key);
    }
  };
  const size_t next_cursor =
      redis_db->ScanKeys(args.cursor, args.count, collect_key);
  client->AddReply(EncodeScanReply(next_cursor, keys));
}
}  // namespace redis_simple::command::key
