#include <string_view>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/reply.h"

namespace redis_simple::commands::keys {
void HandleType(Client* const client) {
  const db::RedisObject* const object =
      client->Db()->LookupKey(client->Args()[0]);
  client->AddReply(reply::FromString(
      object == nullptr ? "none" : db::TypeName(object->Type())));
}
}  // namespace redis_simple::commands::keys
