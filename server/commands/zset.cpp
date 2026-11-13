#include "data_types/zset/zset.h"

#include <algorithm>
#include <cmath>
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
#include "server/commands/lookup.h"
#include "server/commands/scan_options.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/floats.h"
#include "utils/strings.h"

namespace redis_simple::commands::zset {
namespace {
struct AddOptions {
  bool nx{};
  bool xx{};
  bool gt{};
  bool lt{};
  bool ch{};
  bool incr{};
};

// Parses ZADD's leading options, as Redis does: they end at the first
// argument that is not one. Returns the index of the first score.
size_t ParseOptions(const commands::CommandArgs& args,
                    AddOptions* const options) {
  size_t index = 1;
  for (; index < args.size(); ++index) {
    const std::string_view option = args[index];
    if (utils::EqualsIgnoreCase(option, "NX")) {
      options->nx = true;
    } else if (utils::EqualsIgnoreCase(option, "XX")) {
      options->xx = true;
    } else if (utils::EqualsIgnoreCase(option, "CH")) {
      options->ch = true;
    } else if (utils::EqualsIgnoreCase(option, "INCR")) {
      options->incr = true;
    } else if (utils::EqualsIgnoreCase(option, "GT")) {
      options->gt = true;
    } else if (utils::EqualsIgnoreCase(option, "LT")) {
      options->lt = true;
    } else {
      break;
    }
  }
  return index;
}

enum class AddResult : std::uint8_t {
  kAdded,
  kUpdated,
  // Processed without a change: the score was already that value.
  kUnchanged,
  // Skipped by NX, XX, GT, or LT.
  kSkipped,
  // INCR produced NaN.
  kNotANumber,
};

// Redis' zsetAdd: applies one score-member pair under the options, and
// stores the member's resulting score in `*score`.
AddResult Add(data_types::ZSet* const zset, std::string_view member,
              const AddOptions& options, double* const score) {
  const std::optional<double> current = zset->Score(member);
  if (!current.has_value()) {
    if (options.xx) {
      return AddResult::kSkipped;
    }
    zset->InsertOrUpdate(member, *score);
    return AddResult::kAdded;
  }
  if (options.nx) {
    return AddResult::kSkipped;
  }
  if (options.incr) {
    *score += *current;
    if (std::isnan(*score)) {
      return AddResult::kNotANumber;
    }
  }
  if ((options.lt && *score >= *current) ||
      (options.gt && *score <= *current)) {
    return AddResult::kSkipped;
  }
  if (*score == *current) {
    return AddResult::kUnchanged;
  }
  zset->InsertOrUpdate(member, *score);
  return AddResult::kUpdated;
}

// Checks the option combinations and the argument count, in Redis' order,
// and replies with the error when they are invalid.
bool ValidateOptions(Client* const client, const AddOptions& options,
                     size_t remaining) {
  if (remaining == 0 || remaining % 2 != 0) {
    client->AddReply(reply::kSyntaxError);
    return false;
  }
  if (options.nx && options.xx) {
    client->AddReply(reply::FromError(
        "ERR XX and NX options at the same time are not compatible"));
    return false;
  }
  if ((options.gt && options.nx) || (options.lt && options.nx) ||
      (options.gt && options.lt)) {
    client->AddReply(reply::FromError(
        "ERR GT, LT, and/or NX options at the same time are not compatible"));
    return false;
  }
  if (options.incr && remaining > 2) {
    client->AddReply(reply::FromError(
        "ERR INCR option supports a single increment-element pair"));
    return false;
  }
  return true;
}

// What ZADD did, for its reply.
struct AddTotals {
  int64_t added{};
  int64_t updated{};
  // Whether any pair was applied rather than skipped, and the last applied
  // pair's resulting score, which INCR replies with.
  bool processed{};
  double last_score{};
};

// Applies every score-member pair. Returns false after replying with an error
// if INCR produced NaN.
bool AddPairs(Client* const client, data_types::ZSet* const zset,
              const commands::CommandArgs& args, size_t first_score,
              const std::vector<double>& scores, const AddOptions& options,
              AddTotals* const totals) {
  const bool plain =
      !options.nx && !options.xx && !options.gt && !options.lt && !options.incr;
  for (size_t i = 0; i < scores.size(); ++i) {
    const std::string_view member = args[first_score + (2 * i) + 1];
    double score = scores[i];
    if (plain) {
      // One lookup: insert or update, learning which happened.
      bool changed = false;
      const bool inserted = zset->InsertOrUpdate(member, score, &changed);
      totals->added += inserted ? 1 : 0;
      totals->updated += changed && !inserted ? 1 : 0;
      continue;
    }
    switch (Add(zset, member, options, &score)) {
      case AddResult::kAdded:
        ++totals->added;
        break;
      case AddResult::kUpdated:
        ++totals->updated;
        break;
      case AddResult::kUnchanged:
        break;
      case AddResult::kSkipped:
        continue;
      case AddResult::kNotANumber:
        client->AddReply(
            reply::FromError("ERR resulting score is not a number (NaN)"));
        return false;
    }
    totals->processed = true;
    totals->last_score = score;
  }
  return true;
}
}  // namespace

void HandleZAdd(Client* const client) {
  // Redis' zaddGenericCommand, including its order of checks.
  const commands::CommandArgs& args = client->Args();
  AddOptions options;
  const size_t first_score = ParseOptions(args, &options);
  if (!ValidateOptions(client, options, args.size() - first_score)) {
    return;
  }
  // Validate every score before changing anything.
  std::vector<double> scores((args.size() - first_score) / 2);
  for (size_t i = 0; i < scores.size(); ++i) {
    if (!utils::ToDouble(args[first_score + (2 * i)], &scores[i])) {
      client->AddReply(reply::kNotAFloatError);
      return;
    }
  }

  auto [object, wrong_type] = MutableLookupTyped(
      client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (object == nullptr && !options.xx) {
    // LLVM 18 cannot trace unique_ptr ownership through Dict::FindOrInsert.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto created = db::RedisObject::CreateWithZSet(data_types::ZSet::Create());
    object = created.get();
    client->Db()->SetKey(args[0], std::move(created), 0);
  }
  AddTotals totals;
  if (object != nullptr && !AddPairs(client, object->ZSet(), args, first_score,
                                     scores, options, &totals)) {
    return;
  }
  // Like Redis, only a change touches watchers and reaches the AOF.
  if (totals.added > 0 || totals.updated > 0) {
    client->MarkModified(totals.added + totals.updated);
  }
  if (options.incr) {
    if (totals.processed) {
      client->AddReply(reply::FromFloat(totals.last_score, client->Protocol()));
    } else {
      client->AddReply(reply::NullView(client->Protocol()));
    }
    return;
  }
  client->AddReply(reply::FromInt64(options.ch ? totals.added + totals.updated
                                               : totals.added));
}

void HandleZCard(Client* const client) {
  const auto [object, wrong_type] = LookupTyped(
      client->Db(), client->Args()[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const size_t size = object == nullptr ? 0 : object->ZSet()->Size();
  client->AddReply(reply::FromInt64(static_cast<int64_t>(size)));
}

namespace {
using LimitSpec = ::redis_simple::data_types::LimitSpec;
using RangeByRankSpec = ::redis_simple::data_types::RangeByRankSpec;
using RangeByScoreSpec = ::redis_simple::data_types::RangeByScoreSpec;
using ZSetRangeVisitor = ::redis_simple::data_types::ZSetRangeVisitor;

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
bool ParseLexTerm(std::string_view term, data_types::LexBound* const bound,
                  bool* const exclusive) {
  if (term == "-" || term == "+") {
    bound->kind = term == "-" ? data_types::LexBound::Kind::kMinusInfinity
                              : data_types::LexBound::Kind::kPlusInfinity;
    *exclusive = true;
    return true;
  }
  if (!term.starts_with('[') && !term.starts_with('(')) {
    return false;
  }
  bound->kind = data_types::LexBound::Kind::kValue;
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
                        const ZSetRangeVisitor& visitor) {
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
  object->ZSet()->ForEachInRankRange(&spec, visitor);
  return RangeStatus::kOk;
}

RangeStatus RangeByScore(Client* const client, const CommandArgs& args,
                         const RangeOptions& options,
                         const ZSetRangeVisitor& visitor) {
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
  object->ZSet()->ForEachInScoreRange(&spec, visitor);
  return RangeStatus::kOk;
}

RangeStatus RangeByLex(Client* const client, const CommandArgs& args,
                       const RangeOptions& options,
                       const ZSetRangeVisitor& visitor) {
  data_types::RangeByLexSpec spec;
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
  object->ZSet()->ForEachInLexRange(&spec, visitor);
  return RangeStatus::kOk;
}

// Encodes a range's members as they are visited, so a reply is built in one
// pass without copying the members first. The array header needs the count,
// so it is sent as a separate buffer in front of the elements.
class RangeReply {
 public:
  RangeReply(bool with_scores, reply::ProtocolVersion protocol)
      : with_scores_(with_scores), protocol_(protocol) {}

  void Add(std::string_view member, double score) {
    // RESP3 pairs each member with its score in a nested array; RESP2
    // replies with one flat array.
    if (NestedScores()) {
      reply::AppendArrayHeader(2, &elements_);
    }
    reply::AppendBulkString(member, &elements_);
    if (with_scores_) {
      reply::AppendFloat(score, protocol_, &elements_);
    }
    ++count_;
  }

  void Send(Client* const client) && {
    const size_t size = with_scores_ && !NestedScores() ? count_ * 2 : count_;
    client->AddReply(reply::FromArrayHeader(size), std::move(elements_));
  }

 private:
  [[nodiscard]] bool NestedScores() const {
    return with_scores_ && protocol_ == reply::ProtocolVersion::kResp3;
  }

  bool with_scores_;
  reply::ProtocolVersion protocol_;
  size_t count_{};
  std::string elements_;
};

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
  const commands::CommandArgs& args = client->Args();
  RangeOptions options;
  if (const RangeStatus parsed =
          ParseRangeOptions(args, type, direction, &options);
      parsed != RangeStatus::kOk) {
    AddRangeError(client, parsed);
    return;
  }

  RangeReply reply(options.with_scores, client->Protocol());
  // A negative LIMIT offset selects nothing, but the bounds and the key's
  // type are still checked. Capturing only `reply` keeps the visitor inside
  // std::function's inline storage.
  const ZSetRangeVisitor visitor =
      options.empty
          ? ZSetRangeVisitor([](std::string_view, double) {})
          : ZSetRangeVisitor([&reply](std::string_view member, double score) {
              reply.Add(member, score);
            });
  RangeStatus status = RangeStatus::kOk;
  switch (options.type) {
    case RangeType::kAuto:
    case RangeType::kRank:
      status = RangeByRank(client, args, options, visitor);
      break;
    case RangeType::kScore:
      status = RangeByScore(client, args, options, visitor);
      break;
    case RangeType::kLex:
      status = RangeByLex(client, args, options, visitor);
      break;
  }
  if (status != RangeStatus::kOk) {
    AddRangeError(client, status);
    return;
  }
  std::move(reply).Send(client);
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
  const commands::CommandArgs& args = client->Args();
  data_types::RangeByLexSpec spec;
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
  const commands::CommandArgs& args = client->Args();
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

namespace {
// Redis' zrankGenericCommand: an optional WITHSCORE adds the score.
void AddRankReply(Client* const client, bool reverse) {
  const commands::CommandArgs& args = client->Args();
  const bool with_score = args.size() == 3;
  if (with_score && !utils::EqualsIgnoreCase(args[2], "WITHSCORE")) {
    client->AddReply(reply::kSyntaxError);
    return;
  }
  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const data_types::ZSet* const zset =
      object == nullptr ? nullptr : object->ZSet();
  std::optional<size_t> rank =
      zset == nullptr ? std::nullopt : zset->Rank(args[1]);
  if (!rank.has_value()) {
    client->AddReply(with_score ? reply::NullArrayView(client->Protocol())
                                : reply::NullView(client->Protocol()));
    return;
  }
  if (reverse) {
    rank = zset->Size() - 1 - *rank;
  }
  std::string encoded;
  if (with_score) {
    reply::AppendArrayHeader(2, &encoded);
  }
  encoded.append(reply::FromInt64(static_cast<int64_t>(*rank)));
  if (with_score) {
    reply::AppendFloat(zset->Score(args[1]).value_or(0), client->Protocol(),
                       &encoded);
  }
  client->AddReply(std::move(encoded));
}
}  // namespace

void HandleZRank(Client* const client) { AddRankReply(client, false); }

void HandleZRevRank(Client* const client) { AddRankReply(client, true); }

void HandleZRem(Client* const client) {
  const commands::CommandArgs& args = client->Args();
  const auto [object, wrong_type] = MutableLookupTyped(
      client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  int64_t removed = 0;
  if (object != nullptr) {
    data_types::ZSet* const zset = object->ZSet();
    for (size_t i = 1; i < args.size(); ++i) {
      removed += zset->Delete(args[i]) ? 1 : 0;
    }
    // Redis deletes a key once its last member is removed.
    if (zset->Size() == 0) {
      client->Db()->DeleteKey(args[0]);
    }
  }
  if (removed > 0) {
    client->MarkModified(removed);
  }
  client->AddReply(reply::FromInt64(removed));
}

void HandleZScan(Client* const client) {
  size_t cursor = 0;
  ScanOptions options;
  db::RedisObject* const object =
      StartCollectionScan(client, ScanTarget::kZset, &cursor, &options);
  if (object == nullptr) {
    return;
  }
  data_types::ZSet* const zset = object->ZSet();
  const bool skiplist =
      zset->Encoding() == data_types::ZSet::Encoding::kSkiplist;
  ScanReply reply(2 * (skiplist ? options.count : zset->Size()));
  utils::FloatBuffer score_buffer;
  // Redis prints a skiplist's scores with ld2string (17 digits) and a
  // listpack's as the listpack stores them (d2string's text).
  const size_t next_cursor = zset->Scan(
      cursor, options.count,
      [&options, &reply, &score_buffer, skiplist](std::string_view member,
                                                  double score) {
        if (!options.Matches(member)) {
          return;
        }
        reply.Add(member);
        reply.Add(skiplist
                      ? utils::FormatFloatWith17Digits(score, &score_buffer)
                      : utils::FormatFloat(score, &score_buffer));
      });
  std::move(reply).Send(client, next_cursor);
}

void HandleZScore(Client* const client) {
  const commands::CommandArgs& args = client->Args();
  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const std::optional<double> score =
      object == nullptr ? std::nullopt : object->ZSet()->Score(args[1]);
  if (score.has_value()) {
    client->AddReply(reply::FromFloat(*score, client->Protocol()));
  } else {
    client->AddReply(reply::NullView(client->Protocol()));
  }
}
}  // namespace redis_simple::commands::zset
