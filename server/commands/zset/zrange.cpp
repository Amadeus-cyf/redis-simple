#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
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
constexpr std::string_view kMaxVal = "+inf";
constexpr std::string_view kMinVal = "-inf";

enum class RangeMode : std::uint8_t {
  kRank,
  kScore,
};

enum class RangeStatus : std::uint8_t {
  kOk,
  kSyntaxError,
  kWrongType,
};

struct RangeOptions {
  bool by_score{false};
  bool reverse{false};
  bool with_scores{false};
  std::optional<LimitSpec> limit;
};

bool ParseRangeOptions(const CommandArgs& args, RangeMode mode, bool reverse,
                       RangeOptions* const options) {
  RangeOptions parsed;
  parsed.by_score = mode == RangeMode::kScore;
  parsed.reverse = reverse;
  bool has_by_score = parsed.by_score;
  bool has_reverse = parsed.reverse;

  for (size_t i = 3; i < args.size();) {
    const std::string_view option = args[i];
    if (utils::EqualsIgnoreCase(option, kFlagByScore)) {
      if (has_by_score) {
        return false;
      }
      has_by_score = true;
      parsed.by_score = true;
      ++i;
      continue;
    }
    if (utils::EqualsIgnoreCase(option, kFlagReverse)) {
      if (has_reverse) {
        return false;
      }
      has_reverse = true;
      parsed.reverse = true;
      ++i;
      continue;
    }
    if (utils::EqualsIgnoreCase(option, kFlagWithScores)) {
      if (parsed.with_scores) {
        return false;
      }
      parsed.with_scores = true;
      ++i;
      continue;
    }
    if (!utils::EqualsIgnoreCase(option, kFlagLimit) ||
        parsed.limit.has_value() || i + 2 >= args.size()) {
      return false;
    }

    int64_t offset = 0;
    int64_t count = 0;
    if (!utils::ToInt64(args[i + 1], &offset) || offset < 0 ||
        !utils::ToInt64(args[i + 2], &count)) {
      return false;
    }
    parsed.limit.emplace(
        static_cast<size_t>(offset),
        count < 0 ? std::nullopt
                  : std::optional<size_t>(static_cast<size_t>(count)));
    i += 3;
  }

  if (parsed.limit.has_value() && !parsed.by_score) {
    return false;
  }

  *options = parsed;
  return true;
}

bool ParseRankRange(std::string_view start, std::string_view end,
                    RangeByRankSpec* const spec) {
  return utils::ToInt64(start, &spec->min) && utils::ToInt64(end, &spec->max);
}

bool ParseScoreTerm(std::string_view term, double* const value) {
  if (term.empty()) {
    return false;
  }
  if (utils::EqualsIgnoreCase(term, kMinVal)) {
    *value = -std::numeric_limits<double>::infinity();
    return true;
  }
  if (utils::EqualsIgnoreCase(term, kMaxVal)) {
    *value = std::numeric_limits<double>::infinity();
    return true;
  }
  const auto number = term[0] == '(' ? term.substr(1) : term;
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
    return RangeStatus::kSyntaxError;
  }
  ApplyRangeOptions(options, &spec);

  const auto* object = client->Db()->LookupKey(args[0]);
  if (object == nullptr) {
    return RangeStatus::kOk;
  }
  if (object->Type() != db::RedisObject::ObjectType::kZSet) {
    return RangeStatus::kWrongType;
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
    return RangeStatus::kSyntaxError;
  }
  ApplyRangeOptions(options, &spec);

  const auto* object = client->Db()->LookupKey(args[0]);
  if (object == nullptr) {
    return RangeStatus::kOk;
  }
  if (object->Type() != db::RedisObject::ObjectType::kZSet) {
    return RangeStatus::kWrongType;
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
      encoded.append(reply::FromFloat(entry->score, protocol));
    }
  }
  return encoded;
}

void AddRangeError(Client* const client, RangeStatus status) {
  if (status == RangeStatus::kWrongType) {
    client->AddReply(reply::kWrongTypeError);
  } else {
    client->AddReply(reply::kSyntaxError);
  }
}

void AddRangeReply(Client* const client, RangeMode mode, bool reverse) {
  const auto& args = client->Args();
  RangeOptions options;
  if (!ParseRangeOptions(args, mode, reverse, &options)) {
    client->AddReply(reply::kSyntaxError);
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
  const auto& args = client->Args();
  RangeByScoreSpec spec;
  if (!ParseScoreRange(args[1], args[2], &spec)) {
    client->AddReply(reply::kSyntaxError);
    return;
  }

  const auto* object = client->Db()->LookupKey(args[0]);
  if (object == nullptr) {
    client->AddReply(reply::FromInt64(0));
    return;
  }
  if (object->Type() != db::RedisObject::ObjectType::kZSet) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const auto count = ToReplyInteger(object->ZSet()->Count(&spec));
  client->AddReply(count.has_value()
                       ? reply::FromInt64(*count)
                       : reply::FromError("ERR zset count out of range"));
}
}  // namespace redis_simple::command::zsets
