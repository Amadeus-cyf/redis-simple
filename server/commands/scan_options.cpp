#include "server/commands/scan_options.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/command.h"
#include "server/commands/lookup.h"
#include "server/db/redis_object.h"
#include "server/reply.h"
#include "utils/check.h"
#include "utils/strings.h"

namespace redis_simple::commands {
namespace {
// Encoded bytes per element reserved up front, for at most this many.
constexpr size_t kReservedElementBytes = 24;
constexpr size_t kMaxReservedElements = 1024;
}  // namespace

bool ParseScanCursor(Client* const client, std::string_view text,
                     size_t* const cursor) {
  uint64_t value = 0;
  if (!utils::ToUint64(text, &value) ||
      std::cmp_greater(value, std::numeric_limits<size_t>::max())) {
    client->AddReply(reply::FromError("ERR invalid cursor"));
    return false;
  }
  *cursor = static_cast<size_t>(value);
  return true;
}

bool ParseScanOptions(Client* const client, size_t first, ScanTarget target,
                      ScanOptions* const options) {
  const CommandArgs& args = client->Args();
  size_t index = first;
  while (index < args.size()) {
    const bool has_value = index + 1 < args.size();
    const std::string_view option = args[index];
    if (has_value && utils::EqualsIgnoreCase(option, "COUNT")) {
      int64_t count = 0;
      if (!utils::ToInt64(args[index + 1], &count)) {
        client->AddReply(reply::kNotAnIntegerError);
        return false;
      }
      // Redis reports a count below 1 as a syntax error.
      if (count < 1) {
        client->AddReply(reply::kSyntaxError);
        return false;
      }
      options->count = static_cast<size_t>(count);
      index += 2;
    } else if (has_value && utils::EqualsIgnoreCase(option, "MATCH")) {
      options->pattern = args[index + 1];
      options->prefix = utils::GlobLiteralPrefix(*options->pattern);
      index += 2;
    } else if (has_value && target == ScanTarget::kKeyspace &&
               utils::EqualsIgnoreCase(option, "TYPE")) {
      options->filter_type = true;
      options->type = db::TypeFromName(args[index + 1]);
      index += 2;
    } else if (utils::EqualsIgnoreCase(option, "NOVALUES")) {
      if (target != ScanTarget::kHash) {
        client->AddReply(
            reply::FromError("ERR NOVALUES option can only be used in HSCAN"));
        return false;
      }
      options->no_values = true;
      ++index;
    } else {
      client->AddReply(reply::kSyntaxError);
      return false;
    }
  }
  return true;
}

db::RedisObject* StartCollectionScan(Client* const client, ScanTarget target,
                                     size_t* const cursor,
                                     ScanOptions* const options) {
  using ObjectType = db::RedisObject::ObjectType;
  const CommandArgs& args = client->Args();
  if (!ParseScanCursor(client, args[1], cursor)) {
    return nullptr;
  }
  ObjectType type = ObjectType::kSet;
  switch (target) {
    case ScanTarget::kHash:
      type = ObjectType::kHash;
      break;
    case ScanTarget::kSet:
      type = ObjectType::kSet;
      break;
    case ScanTarget::kZset:
      type = ObjectType::kZSet;
      break;
    case ScanTarget::kKeyspace:
      utils::Unreachable();
  }
  const auto [object, wrong_type] =
      MutableLookupTyped(client->Db(), args[0], type);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return nullptr;
  }
  if (object == nullptr) {
    client->AddReply(kEmptyScanReply);
    return nullptr;
  }
  if (!ParseScanOptions(client, 2, target, options)) {
    return nullptr;
  }
  return object;
}

ScanReply::ScanReply(size_t expected) {
  elements_.reserve(std::min(expected, kMaxReservedElements) *
                    kReservedElementBytes);
}

void ScanReply::Send(Client* const client, size_t cursor) && {
  constexpr size_t kHeaderBytes = 64;
  std::string header;
  header.reserve(kHeaderBytes);
  reply::AppendArrayHeader(2, &header);
  std::array<char, 21> cursor_buffer{};
  const int cursor_length =
      utils::Uint64ToString(cursor_buffer, static_cast<uint64_t>(cursor));
  reply::AppendBulkString(std::string_view(cursor_buffer.data(),
                                           static_cast<size_t>(cursor_length)),
                          &header);
  reply::AppendArrayHeader(count_, &header);
  client->AddReply(std::move(header), std::move(elements_));
}
}  // namespace redis_simple::commands
