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

constexpr std::string_view kFlagByScore = "BYSCORE";
constexpr std::string_view kFlagLimit = "LIMIT";
constexpr std::string_view kFlagReverse = "REV";
constexpr std::string_view kFlagWithScores = "WITHSCORES";
constexpr std::string_view kMaxValue = "+inf";
constexpr std::string_view kMinValue = "-inf";

enum class RangeMode : std::uint8_t {
  kRank,
  kScore,
};

enum class RangeStatus : std::uint8_t {
  kOk,
  kSyntaxError,
  kLimitWithoutScore,
  kNotAnInteger,
  kNotAFloat,
  kWrongType,
};

struct RangeOptions {
  bool by_score{false};
  bool reverse{false};
  bool with_scores{false};
  // A negative LIMIT offset, which Redis answers with an empty range.
  bool empty{false};
  std::optional<LimitSpec> limit;
};

// Parses options in Redis' order: syntax errors and LIMIT values first, then
// a LIMIT used without BYSCORE. WITHSCORES and LIMIT may repeat, and the last
// LIMIT wins; BYSCORE and REV may not.
RangeStatus ParseRangeOptions(const CommandArgs& args, RangeMode mode,
                              bool reverse, RangeOptions* const options) {
  RangeOptions parsed;
  parsed.by_score = mode == RangeMode::kScore;
  parsed.reverse = reverse;
  bool has_by_score = parsed.by_score;
  bool has_reverse = parsed.reverse;
  bool limit_requires_score = false;

  for (size_t i = 3; i < args.size();) {
    const std::string_view option = args[i];
    if (utils::EqualsIgnoreCase(option, kFlagByScore)) {
      if (has_by_score) {
        return RangeStatus::kSyntaxError;
      }
      has_by_score = true;
      parsed.by_score = true;
      ++i;
      continue;
    }
    if (utils::EqualsIgnoreCase(option, kFlagReverse)) {
      if (has_reverse) {
        return RangeStatus::kSyntaxError;
      }
      has_reverse = true;
      parsed.reverse = true;
      ++i;
      continue;
    }
    if (utils::EqualsIgnoreCase(option, kFlagWithScores)) {
      parsed.with_scores = true;
      ++i;
      continue;
    }
    if (!utils::EqualsIgnoreCase(option, kFlagLimit) || i + 2 >= args.size()) {
      return RangeStatus::kSyntaxError;
    }

    int64_t offset = 0;
    int64_t count = 0;
    if (!utils::ToInt64(args[i + 1], &offset) ||
        !utils::ToInt64(args[i + 2], &count)) {
      return RangeStatus::kNotAnInteger;
    }
    parsed.empty = offset < 0;
    // Like Redis, only a count other than -1 conflicts with a rank range.
    limit_requires_score = count != -1;
    parsed.limit.emplace(
        static_cast<size_t>(std::max<int64_t>(offset, 0)),
        count < 0 ? std::nullopt
                  : std::optional<size_t>(static_cast<size_t>(count)));
    i += 3;
  }

  if (parsed.limit.has_value() && !parsed.by_score) {
    if (limit_requires_score) {
      return RangeStatus::kLimitWithoutScore;
    }
    // A rank range ignores LIMIT entirely, including its offset.
    parsed.limit.reset();
    parsed.empty = false;
  }

  *options = parsed;
  return RangeStatus::kOk;
}

bool ParseRankRange(std::string_view start, std::string_view end,
                    RangeByRankSpec* const spec) {
  return utils::ToInt64(start, &spec->min) && utils::ToInt64(end, &spec->max);
}

bool ParseScoreTerm(std::string_view term, double* const value) {
  if (term.empty()) {
    return false;
  }
  if (utils::EqualsIgnoreCase(term, kMinValue)) {
    *value = -std::numeric_limits<double>::infinity();
    return true;
  }
  if (utils::EqualsIgnoreCase(term, kMaxValue)) {
    *value = std::numeric_limits<double>::infinity();
    return true;
  }
  const std::basic_string_view<char> number =
      term[0] == '(' ? term.substr(1) : term;
  return utils::ToDouble(number, value);
}

bool ParseScoreRange(std::string_view start, std::string_view end,
                     RangeByScoreSpec* const spec) {
  if (!ParseScoreTerm(start, &spec->min) || !ParseScoreTerm(end, &spec->max)) {
    return false;
  }
  spec->minex = start[0] == '(';
  spec->maxex = end[0] == '(';
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

void AddRangeReply(Client* const client, RangeMode mode, bool reverse) {
  const command::CommandArgs& args = client->Args();
  RangeOptions options;
  if (const RangeStatus parsed =
          ParseRangeOptions(args, mode, reverse, &options);
      parsed != RangeStatus::kOk) {
    AddRangeError(client, parsed);
    return;
  }

  ZSetEntryList entries;
  const RangeStatus status = options.by_score
                                 ? RangeByScore(client, args, options, &entries)
                                 : RangeByRank(client, args, options, &entries);
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
  AddRangeReply(client, RangeMode::kRank, false);
}

void HandleZRevRange(Client* const client) {
  AddRangeReply(client, RangeMode::kRank, true);
}

void HandleZRangeByScore(Client* const client) {
  AddRangeReply(client, RangeMode::kScore, false);
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
