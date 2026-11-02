#include <string_view>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/reply.h"

namespace redis_simple::command::transaction {
void HandleMulti(Client* const client) {
  if (client->InTransaction()) {
    client->AddReply(reply::FromError("ERR MULTI calls can not be nested"));
    return;
  }
  client->BeginTransaction();
  client->AddReply(reply::kOk);
}

void HandleExec(Client* const client) {
  if (!client->InTransaction()) {
    client->AddReply(reply::FromError("ERR EXEC without MULTI"));
    return;
  }
  client->ExecTransaction();
}

void HandleDiscard(Client* const client) {
  if (!client->InTransaction()) {
    client->AddReply(reply::FromError("ERR DISCARD without MULTI"));
    return;
  }
  client->DiscardTransaction();
  client->AddReply(reply::kOk);
}

void HandleWatch(Client* const client) {
  if (client->InTransaction()) {
    client->AddReply(reply::FromError("ERR WATCH inside MULTI is not allowed"));
    return;
  }
  for (const std::string_view key : client->Args()) {
    client->WatchKey(key);
  }
  client->AddReply(reply::kOk);
}

void HandleUnwatch(Client* const client) {
  client->UnwatchKeys();
  client->AddReply(reply::kOk);
}
}  // namespace redis_simple::command::transaction
