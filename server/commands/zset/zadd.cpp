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
}  // namespace

void HandleZAdd(Client* const client) {
  // Redis' zaddGenericCommand, including its order of checks.
  const command::CommandArgs& args = client->Args();
  AddOptions options;
  const size_t first_score = ParseOptions(args, &options);
  const size_t remaining = args.size() - first_score;
  if (remaining == 0 || remaining % 2 != 0) {
    client->AddReply(reply::kSyntaxError);
    return;
  }
  if (options.nx && options.xx) {
    client->AddReply(reply::FromError(
        "ERR XX and NX options at the same time are not compatible"));
    return;
  }
  if ((options.gt && options.nx) || (options.lt && options.nx) ||
      (options.gt && options.lt)) {
    client->AddReply(reply::FromError(
        "ERR GT, LT, and/or NX options at the same time are not compatible"));
    return;
  }
  const size_t pairs = remaining / 2;
  if (options.incr && pairs > 1) {
    client->AddReply(reply::FromError(
        "ERR INCR option supports a single increment-element pair"));
    return;
  }
  // Validate every score before mutating anything.
  std::vector<double> scores(pairs);
  for (size_t i = 0; i < pairs; ++i) {
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
  int64_t added = 0;
  int64_t updated = 0;
  bool processed = false;
  double last_score = 0;
  if (object == nullptr && !options.xx) {
    // LLVM 18 cannot trace unique_ptr ownership through Dict::FindOrInsert.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto created = db::RedisObject::CreateWithZSet(zset::ZSet::Create());
    object = created.get();
    client->Db()->SetKey(args[0], std::move(created), 0);
  }
  if (object != nullptr) {
    zset::ZSet* const zset = object->ZSet();
    const bool plain = !options.nx && !options.xx && !options.gt &&
                       !options.lt && !options.incr;
    for (size_t i = 0; i < pairs; ++i) {
      const std::string_view member = args[first_score + (2 * i) + 1];
      double score = scores[i];
      if (plain) {
        // One lookup: insert or update, learning which happened.
        bool changed = false;
        const bool inserted = zset->InsertOrUpdate(member, score, &changed);
        added += inserted ? 1 : 0;
        updated += changed && !inserted ? 1 : 0;
        continue;
      }
      switch (Add(zset, member, options, &score)) {
        case AddResult::kAdded:
          ++added;
          break;
        case AddResult::kUpdated:
          ++updated;
          break;
        case AddResult::kUnchanged:
          break;
        case AddResult::kSkipped:
          continue;
        case AddResult::kNotANumber:
          client->AddReply(
              reply::FromError("ERR resulting score is not a number (NaN)"));
          return;
      }
      processed = true;
      last_score = score;
    }
  }
  // Like Redis, only a change touches watchers and reaches the AOF.
  if (added > 0 || updated > 0) {
    client->MarkModified();
  }
  if (options.incr) {
    if (processed) {
      client->AddReply(reply::FromFloat(last_score, client->Protocol()));
    } else {
      client->AddReply(reply::NullView(client->Protocol()));
    }
    return;
  }
  client->AddReply(reply::FromInt64(options.ch ? added + updated : added));
}
}  // namespace redis_simple::command::zsets
