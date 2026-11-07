#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"
#include "utils/float_utils.h"
#include "utils/string_utils.h"

namespace redis_simple::command::zsets {
namespace {
using LimitSpec = ::redis_simple::zset::LimitSpec;
using RangeByRankSpec = ::redis_simple::zset::RangeByRankSpec;
using RangeByScoreSpec = ::redis_simple::zset::RangeByScoreSpec;
using ZSetEntryList = ::redis_simple::zset::ZSetEntryList;

constexpr std::string_view kFlagByLex = "BYLEX";
constexpr std::string_view kFlagByScore = "BYSCORE";
constexpr std::string_view kFlagLimit = "LIMIT";
constexpr std::string_view kFlagReverse = "REV";
constexpr std::string_view kFlagWithScores = "WITHSCORES";

// What a command fixes, as in Redis' zrangeGenericCommand: ZRANGE leaves
// the range type and direction to its options; the others set them.
enum class RangeType : std::uint8_t {
  kAuto,
  kRank,
  kScore,
  kLex,
};

enum class Direction : std::uint8_t {
  kAuto,
  kForward,
  kReverse,
};

enum class RangeStatus : std::uint8_t {
  kOk,
  kSyntaxError,
  kLimitWithoutScore,
  kWithScoresByLex,
  kNotAnInteger,
  kNotAFloat,
  kNotALexItem,
  kWrongType,
};

struct RangeOptions {
  RangeType type{RangeType::kRank};
  bool reverse{false};
  bool with_scores{false};
  // A negative LIMIT offset, which Redis answers with an empty range.
  bool empty{false};
  std::optional<LimitSpec> limit;
};

// Parses options as Redis does: WITHSCORES and LIMIT anywhere and
// repeatable, REV, BYSCORE, and BYLEX only where the command leaves them
// open; then LIMIT with a rank range, and WITHSCORES with BYLEX, are errors.
RangeStatus ParseRangeOptions(const CommandArgs& args, RangeType type,
                              Direction direction,
                              RangeOptions* const options) {
  RangeOptions parsed;
  int64_t count = -1;
  for (size_t i = 3; i < args.size(); ++i) {
    const std::string_view option = args[i];
    const size_t remaining = args.size() - i - 1;
    if (utils::EqualsIgnoreCase(option, kFlagWithScores)) {
      parsed.with_scores = true;
    } else if (utils::EqualsIgnoreCase(option, kFlagLimit) && remaining >= 2) {
      int64_t offset = 0;
      if (!utils::ToInt64(args[i + 1], &offset) ||
          !utils::ToInt64(args[i + 2], &count)) {
        return RangeStatus::kNotAnInteger;
      }
      parsed.empty = offset < 0;
      parsed.limit.emplace(
          static_cast<size_t>(std::max<int64_t>(offset, 0)),
          count < 0 ? std::nullopt
                    : std::optional<size_t>(static_cast<size_t>(count)));
      i += 2;
    } else if (direction == Direction::kAuto &&
               utils::EqualsIgnoreCase(option, kFlagReverse)) {
      direction = Direction::kReverse;
    } else if (type == RangeType::kAuto &&
               utils::EqualsIgnoreCase(option, kFlagByLex)) {
      type = RangeType::kLex;
    } else if (type == RangeType::kAuto &&
               utils::EqualsIgnoreCase(option, kFlagByScore)) {
      type = RangeType::kScore;
    } else {
      return RangeStatus::kSyntaxError;
    }
  }
  parsed.type = type == RangeType::kAuto ? RangeType::kRank : type;
  parsed.reverse = direction == Direction::kReverse;
  if (parsed.type == RangeType::kRank && parsed.limit.has_value()) {
    // Only a count other than -1 conflicts with a rank range, which
    // otherwise ignores LIMIT entirely, including its offset.
    if (count != -1) {
      return RangeStatus::kLimitWithoutScore;
    }
    parsed.limit.reset();
    parsed.empty = false;
  }
  if (parsed.with_scores && parsed.type == RangeType::kLex) {
    return RangeStatus::kWithScoresByLex;
  }
  *options = parsed;
  return RangeStatus::kOk;
}

bool ParseRankRange(std::string_view start, std::string_view end,
                    RangeByRankSpec* const spec) {
  return utils::ToInt64(start, &spec->min) && utils::ToInt64(end, &spec->max);
}

// Redis' zslParseRange: "(" makes a bound exclusive, and the rest parses
// like strtod, so "-inf", "+inf", "", and "1e400" are all bounds.
bool ParseScoreTerm(std::string_view term, double* const value,
                    bool* const exclusive) {
  *exclusive = term.starts_with('(');
  return utils::ToRangeDouble(*exclusive ? term.substr(1) : term, value);
}

bool ParseScoreRange(std::string_view start, std::string_view end,
                     RangeByScoreSpec* const spec) {
  return ParseScoreTerm(start, &spec->min, &spec->minex) &&
         ParseScoreTerm(end, &spec->max, &spec->maxex);
}

// Redis' zslParseLexRangeItem: "[" or "(" and a member, or "-" or "+".
bool ParseLexTerm(std::string_view term, zset::LexBound* const bound,
                  bool* const exclusive) {
  if (term == "-" || term == "+") {
    bound->kind = term == "-" ? zset::LexBound::Kind::kMinusInfinity
                              : zset::LexBound::Kind::kPlusInfinity;
    *exclusive = true;
    return true;
  }
  if (!term.starts_with('[') && !term.starts_with('(')) {
    return false;
  }
  bound->kind = zset::LexBound::Kind::kValue;
  bound->value = term.substr(1);
  *exclusive = term.front() == '(';
  return true;
}

template <typename Spec>
void ApplyRangeOptions(const RangeOptions& options, Spec* const spec) {
  spec->reverse = options.reverse;
  if (options.limit.has_value()) {
    spec->limit = options.limit;
  }
}

RangeStatus RangeByRank(Client* const client, const CommandArgs& args,
                        const RangeOptions& options,
                        ZSetEntryList* const result) {
  RangeByRankSpec spec;
  if (!ParseRankRange(args[1], args[2], &spec)) {
    return RangeStatus::kNotAnInteger;
  }
  ApplyRangeOptions(options, &spec);

  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    return RangeStatus::kWrongType;
  }
  if (object == nullptr) {
    return RangeStatus::kOk;
  }
  *result = object->ZSet()->RangeByRank(&spec);
  return RangeStatus::kOk;
}

RangeStatus RangeByScore(Client* const client, const CommandArgs& args,
                         const RangeOptions& options,
                         ZSetEntryList* const result) {
  RangeByScoreSpec spec;
  const std::string_view start = options.reverse ? args[2] : args[1];
  const std::string_view stop = options.reverse ? args[1] : args[2];
  if (!ParseScoreRange(start, stop, &spec)) {
    return RangeStatus::kNotAFloat;
  }
  ApplyRangeOptions(options, &spec);

  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    return RangeStatus::kWrongType;
  }
  if (object == nullptr) {
    return RangeStatus::kOk;
  }
  *result = object->ZSet()->RangeByScore(&spec);
  return RangeStatus::kOk;
}

RangeStatus RangeByLex(Client* const client, const CommandArgs& args,
                       const RangeOptions& options,
                       ZSetEntryList* const result) {
  zset::RangeByLexSpec spec;
  const std::string_view start = options.reverse ? args[2] : args[1];
  const std::string_view stop = options.reverse ? args[1] : args[2];
  if (!ParseLexTerm(start, &spec.min, &spec.minex) ||
      !ParseLexTerm(stop, &spec.max, &spec.maxex)) {
    return RangeStatus::kNotALexItem;
  }
  ApplyRangeOptions(options, &spec);

  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    return RangeStatus::kWrongType;
  }
  if (object == nullptr) {
    return RangeStatus::kOk;
  }
  *result = object->ZSet()->RangeByLex(&spec);
  return RangeStatus::kOk;
}

std::string EncodeZRangeReply(const ZSetEntryList& result, bool with_scores,
                              reply::ProtocolVersion protocol) {
  const bool nested_scores =
      with_scores && protocol == reply::ProtocolVersion::kResp3;
  const size_t reply_size =
      with_scores && !nested_scores ? result.size() * 2 : result.size();
  std::string encoded = reply::FromArrayHeader(reply_size);
  for (const auto* entry : result) {
    if (nested_scores) {
      encoded.append(reply::FromArrayHeader(2));
    }
    reply::AppendBulkString(entry->key, &encoded);
    if (with_scores) {
      reply::AppendFloat(entry->score, protocol, &encoded);
    }
  }
  return encoded;
}

void AddRangeError(Client* const client, RangeStatus status) {
  switch (status) {
    case RangeStatus::kOk:
      return;
    case RangeStatus::kSyntaxError:
      client->AddReply(reply::kSyntaxError);
      return;
    case RangeStatus::kLimitWithoutScore:
      client->AddReply(reply::FromError(
          "ERR syntax error, LIMIT is only supported in combination with "
          "either BYSCORE or BYLEX"));
      return;
    case RangeStatus::kWithScoresByLex:
      client->AddReply(reply::FromError(
          "ERR syntax error, WITHSCORES not supported in combination with "
          "BYLEX"));
      return;
    case RangeStatus::kNotALexItem:
      client->AddReply(
          reply::FromError("ERR min or max not valid string range item"));
      return;
    case RangeStatus::kNotAnInteger:
      client->AddReply(reply::kNotAnIntegerError);
      return;
    case RangeStatus::kNotAFloat:
      client->AddReply(reply::kMinMaxNotFloatError);
      return;
    case RangeStatus::kWrongType:
      client->AddReply(reply::kWrongTypeError);
      return;
  }
}

void AddRangeReply(Client* const client, RangeType type, Direction direction) {
  const command::CommandArgs& args = client->Args();
  RangeOptions options;
  if (const RangeStatus parsed =
          ParseRangeOptions(args, type, direction, &options);
      parsed != RangeStatus::kOk) {
    AddRangeError(client, parsed);
    return;
  }

  ZSetEntryList entries;
  RangeStatus status = RangeStatus::kOk;
  switch (options.type) {
    case RangeType::kAuto:
    case RangeType::kRank:
      status = RangeByRank(client, args, options, &entries);
      break;
    case RangeType::kScore:
      status = RangeByScore(client, args, options, &entries);
      break;
    case RangeType::kLex:
      status = RangeByLex(client, args, options, &entries);
      break;
  }
  if (status != RangeStatus::kOk) {
    AddRangeError(client, status);
    return;
  }
  if (options.empty) {
    entries.clear();
  }
  client->AddReply(
      EncodeZRangeReply(entries, options.with_scores, client->Protocol()));
}

std::optional<int64_t> ToReplyInteger(size_t value) {
  if (value > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}
}  // namespace

void HandleZRange(Client* const client) {
  AddRangeReply(client, RangeType::kAuto, Direction::kAuto);
}

void HandleZRevRange(Client* const client) {
  AddRangeReply(client, RangeType::kRank, Direction::kReverse);
}

void HandleZRangeByScore(Client* const client) {
  AddRangeReply(client, RangeType::kScore, Direction::kForward);
}

void HandleZRevRangeByScore(Client* const client) {
  AddRangeReply(client, RangeType::kScore, Direction::kReverse);
}

void HandleZRangeByLex(Client* const client) {
  AddRangeReply(client, RangeType::kLex, Direction::kForward);
}

void HandleZRevRangeByLex(Client* const client) {
  AddRangeReply(client, RangeType::kLex, Direction::kReverse);
}

void HandleZLexCount(Client* const client) {
  // Redis' zlexcountCommand: the range first, then the key.
  const command::CommandArgs& args = client->Args();
  zset::RangeByLexSpec spec;
  if (!ParseLexTerm(args[1], &spec.min, &spec.minex) ||
      !ParseLexTerm(args[2], &spec.max, &spec.maxex)) {
    client->AddReply(
        reply::FromError("ERR min or max not valid string range item"));
    return;
  }
  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const size_t count = object == nullptr ? 0 : object->ZSet()->LexCount(&spec);
  const std::optional<int64_t> reply_count = ToReplyInteger(count);
  client->AddReply(reply_count.has_value()
                       ? reply::FromInt64(*reply_count)
                       : reply::FromError("ERR zset count out of range"));
}

void HandleZCount(Client* const client) {
  const command::CommandArgs& args = client->Args();
  RangeByScoreSpec spec;
  if (!ParseScoreRange(args[1], args[2], &spec)) {
    client->AddReply(reply::kMinMaxNotFloatError);
    return;
  }

  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (object == nullptr) {
    client->AddReply(reply::FromInt64(0));
    return;
  }
  const std::optional<int64_t> count =
      ToReplyInteger(object->ZSet()->Count(&spec));
  client->AddReply(count.has_value()
                       ? reply::FromInt64(*count)
                       : reply::FromError("ERR zset count out of range"));
}
}  // namespace redis_simple::command::zsets
