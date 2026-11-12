#include "server/aof/aof.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/rdb/rdb.h"
#include "server/reply.h"
#include "utils/string_utils.h"

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
  // Inside EXEC, or while a background save runs: the cron starts it once
  // the transaction is done and no child runs.
  const rdb::Rdb* const rdb = client->Server().rdb;
  if (client->InTransaction() ||
      (rdb != nullptr && rdb->BackgroundSaveInProgress())) {
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
  if (rdb != nullptr && rdb->BackgroundSaveInProgress()) {
    client->AddReply(
        reply::FromError("ERR Background save already in progress"));
    return;
  }
  // Redis replies with a bare "-ERR" when the save fails; the log says why.
  if (rdb == nullptr || !rdb->Save(client->Db())) {
    client->AddReply(reply::FromError("ERR"));
    return;
  }
  client->AddReply(reply::kOk);
}

void HandleBgSave(Client* const client) {
  // BGSAVE [SCHEDULE], with the checks and replies of Redis' bgsaveCommand.
  const commands::CommandArgs& args = client->Args();
  const bool schedule =
      args.size() == 1 && utils::EqualsIgnoreCase(args[0], "SCHEDULE");
  if (!args.empty() && !schedule) {
    client->AddReply(reply::kSyntaxError);
    return;
  }
  rdb::Rdb* const rdb = client->Server().rdb;
  if (rdb == nullptr) {
    client->AddReply(reply::FromError("ERR"));
    return;
  }
  if (rdb->BackgroundSaveInProgress()) {
    client->AddReply(
        reply::FromError("ERR Background save already in progress"));
    return;
  }
  const aof::Aof* const aof = client->Aof();
  if ((aof != nullptr && aof->RewriteInProgress()) || client->InTransaction()) {
    if (!schedule && !client->InTransaction()) {
      client->AddReply(reply::FromError(
          "ERR Another child process is active (AOF?): can't BGSAVE right "
          "now. Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
          "possible."));
      return;
    }
    rdb->ScheduleBackgroundSave();
    client->AddReply(reply::FromString("Background saving scheduled"));
    return;
  }
  if (rdb->StartBackgroundSave(client->Db()) ==
      rdb::BackgroundSaveResult::kStarted) {
    client->AddReply(reply::FromString("Background saving started"));
    return;
  }
  client->AddReply(reply::FromError("ERR"));
}

void HandleLastSave(Client* const client) {
  const rdb::Rdb* const rdb = client->Server().rdb;
  client->AddReply(reply::FromInt64(rdb == nullptr ? 0 : rdb->LastSaveTime()));
}
}  // namespace redis_simple::commands::persistence
