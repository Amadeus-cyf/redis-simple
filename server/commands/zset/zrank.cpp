#include <cstddef>
#include <cstdint>
#include <optional>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"

namespace redis_simple::command::zsets {
void HandleZRank(Client* const client) {
  const auto& args = client->Args();
  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const std::optional<size_t> rank =
      object == nullptr ? std::nullopt : object->ZSet()->Rank(args[1]);
  if (rank.has_value()) {
    client->AddReply(reply::FromInt64(static_cast<int64_t>(*rank)));
  } else {
    client->AddReply(reply::NullView(client->Protocol()));
  }
}
}  // namespace redis_simple::command::zsets
