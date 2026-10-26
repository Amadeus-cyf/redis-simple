#include "data_types/set/set.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"

namespace redis_simple::command::sets {
void HandleSIsMember(Client* const client) {
  const auto& args = client->Args();
  const auto [object, wrong_type] =
      LookupTyped(client->Db(), args[0], db::RedisObject::ObjectType::kSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  const bool member = object != nullptr && object->Set()->HasMember(args[1]);
  client->AddReply(reply::FromInt64(member ? 1 : 0));
}
}  // namespace redis_simple::command::sets
