#include <string_view>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/db/redis_obj.h"
#include "server/reply.h"

namespace redis_simple::command::key {
namespace {
std::string_view ObjectTypeName(const db::RedisObject* const object) {
  if (object == nullptr) {
    return "none";
  }
  switch (object->Type()) {
    case db::RedisObject::ObjectType::kString:
      return "string";
    case db::RedisObject::ObjectType::kSet:
      return "set";
    case db::RedisObject::ObjectType::kList:
      return "list";
    case db::RedisObject::ObjectType::kZSet:
      return "zset";
    case db::RedisObject::ObjectType::kHash:
      return "hash";
  }
  return "none";
}
}  // namespace

void HandleType(Client* const client) {
  client->AddReply(reply::FromString(
      ObjectTypeName(client->Db()->LookupKey(client->Args()[0]))));
}
}  // namespace redis_simple::command::key
