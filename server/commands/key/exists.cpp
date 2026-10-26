#include <cstdint>
#include <string_view>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/db/db.h"
#include "server/reply.h"

namespace redis_simple::command::key {
void HandleExists(Client* const client) {
  int64_t existing = 0;
  for (const std::string_view key : client->Args()) {
    if (client->Db()->LookupKey(key) != nullptr) {
      ++existing;
    }
  }
  client->AddReply(reply::FromInt64(existing));
}
}  // namespace redis_simple::command::key
