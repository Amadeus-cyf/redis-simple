#include <cstddef>
#include <cstdint>

#include "data_types/set/set.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"

namespace redis_simple::command::sets {
void HandleSRem(Client* const client) {
  const auto& args = client->Args();
  const auto [object, wrong_type] = MutableLookupTyped(
      client->Db(), args[0], db::RedisObject::ObjectType::kSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  int64_t removed = 0;
  if (object != nullptr) {
    set::Set* const set = object->Set();
    for (size_t i = 1; i < args.size(); ++i) {
      removed += set->Remove(args[i]) ? 1 : 0;
    }
  }
  if (removed > 0) {
    client->MarkModified();
  }
  client->AddReply(reply::FromInt64(removed));
}
}  // namespace redis_simple::command::sets
