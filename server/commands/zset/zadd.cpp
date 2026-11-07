#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/float_utils.h"
#include "utils/string_utils.h"

namespace redis_simple::command::zsets {
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
size_t ParseOptions(const command::CommandArgs& args,
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
AddResult Add(zset::ZSet* const zset, std::string_view member,
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
bool AddPairs(Client* const client, zset::ZSet* const zset,
              const command::CommandArgs& args, size_t first_score,
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
  const command::CommandArgs& args = client->Args();
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
    auto created = db::RedisObject::CreateWithZSet(zset::ZSet::Create());
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
    client->MarkModified();
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
}  // namespace redis_simple::command::zsets
