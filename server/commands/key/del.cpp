#include <cstdint>
#include <string_view>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/reply.h"

namespace redis_simple::command::key {
namespace {
using DeleteOperation = db::DbStatus (db::RedisDb::*)(std::string_view);

void AddDeleteReply(Client* const client, DeleteOperation operation) {
  db::RedisDb* const redis_db = client->Db();
  int64_t deleted = 0;
  for (const std::string_view key : client->Args()) {
    if ((redis_db->*operation)(key) == db::DbStatus::kOk) {
      ++deleted;
    }
  }
  if (deleted > 0) {
    client->MarkModified();
  }
  client->AddReply(reply::FromInt64(deleted));
}
}  // namespace

void HandleDel(Client* const client) {
  AddDeleteReply(client, &db::RedisDb::DeleteKey);
}

void HandleUnlink(Client* const client) {
  AddDeleteReply(client, &db::RedisDb::UnlinkKey);
}
}  // namespace redis_simple::command::key
