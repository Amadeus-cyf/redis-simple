#include <optional>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"

namespace redis_simple::command::zsets {
void HandleZScore(Client* const client) {
  const auto& args = client->Args();
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
}  // namespace redis_simple::command::zsets
