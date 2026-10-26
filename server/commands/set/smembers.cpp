#include <string>
#include <string_view>
#include <utility>

#include "data_types/set/set.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/reply.h"

namespace redis_simple::command::sets {
void HandleSMembers(Client* const client) {
  const auto [object, wrong_type] = LookupTyped(
      client->Db(), client->Args()[0], db::RedisObject::ObjectType::kSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (object == nullptr) {
    client->AddReply(reply::FromSetHeader(0, client->Protocol()));
    return;
  }
  const set::Set* const set = object->Set();
  std::string encoded = reply::FromSetHeader(set->Size(), client->Protocol());
  set->ForEachMember([&encoded](std::string_view member) {
    reply::AppendBulkString(member, &encoded);
    return true;
  });
  client->AddReply(std::move(encoded));
}
}  // namespace redis_simple::command::sets
