#include <cstddef>
#include <cstdint>

#include "data_types/set/set.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"

namespace redis_simple::command::sets {
void HandleSCard(Client* const client) {
  const auto [object, wrong_type] = LookupTyped(
      client->Db(), client->Args()[0], db::RedisObject::ObjectType::kSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const size_t size = object == nullptr ? 0 : object->Set()->Size();
  client->AddReply(reply::FromInt64(static_cast<int64_t>(size)));
}
}  // namespace redis_simple::command::sets
