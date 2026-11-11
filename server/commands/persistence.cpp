#include "server/aof/aof.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/rdb/rdb.h"
#include "server/reply.h"

namespace redis_simple::commands::persistence {
void HandleBgRewriteAof(Client* const client) {
  auto* const aof = client->Aof();
  if (aof == nullptr) {
    client->AddReply(reply::FromError("ERR append only file is disabled"));
    return;
  }

  // The same checks and replies as Redis' bgrewriteaofCommand.
  if (aof->RewriteInProgress()) {
    client->AddReply(reply::FromError(
        "ERR Background append only file rewriting already in progress"));
    return;
  }
  if (client->InTransaction()) {
    // Inside EXEC: the cron starts it once the transaction is done.
    aof->ScheduleRewrite();
    client->AddReply(
        reply::FromString("Background append only file rewriting scheduled"));
    return;
  }
  if (aof->StartRewrite(client->Db()) == aof::RewriteResult::kStarted) {
    client->AddReply(
        reply::FromString("Background append only file rewriting started"));
    return;
  }
  client->AddReply(reply::FromError(
      "ERR Can't execute an AOF background rewriting. Please check the server "
      "logs for more information."));
}

void HandleSave(Client* const client) {
  rdb::Rdb* const rdb = client->Server().rdb;
  // Redis replies with a bare "-ERR" when the save fails; the log says why.
  if (rdb == nullptr || !rdb->Save(client->Db())) {
    client->AddReply(reply::FromError("ERR"));
    return;
  }
  client->AddReply(reply::kOk);
}

void HandleLastSave(Client* const client) {
  const rdb::Rdb* const rdb = client->Server().rdb;
  client->AddReply(reply::FromInt64(rdb == nullptr ? 0 : rdb->LastSaveTime()));
}
}  // namespace redis_simple::commands::persistence
