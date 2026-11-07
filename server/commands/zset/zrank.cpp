#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple::command::zsets {
namespace {
// Redis' zrankGenericCommand: an optional WITHSCORE adds the score.
void AddRankReply(Client* const client, bool reverse) {
  const command::CommandArgs& args = client->Args();
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
  const zset::ZSet* const zset = object == nullptr ? nullptr : object->ZSet();
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
}  // namespace redis_simple::command::zsets
