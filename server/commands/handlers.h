#ifndef REDIS_SIMPLE_SERVER_COMMANDS_HANDLERS_H_
#define REDIS_SIMPLE_SERVER_COMMANDS_HANDLERS_H_

namespace redis_simple {
class Client;
}  // namespace redis_simple

namespace redis_simple::commands::keys {
void HandleDbSize(Client* client);
void HandleDel(Client* client);
void HandleExpire(Client* client);
void HandleExpireAt(Client* client);
void HandleExists(Client* client);
void HandleFlushDb(Client* client);
void HandlePersist(Client* client);
void HandlePExpire(Client* client);
void HandlePExpireAt(Client* client);
void HandlePTtl(Client* client);
void HandleRename(Client* client);
void HandleScan(Client* client);
void HandleTtl(Client* client);
void HandleType(Client* client);
void HandleUnlink(Client* client);
}  // namespace redis_simple::commands::keys

namespace redis_simple::commands::session {
void HandleAuth(Client* client);
void HandleEcho(Client* client);
void HandleHello(Client* client);
void HandleClient(Client* client);
void HandleSelect(Client* client);
void HandlePing(Client* client);
void HandleQuit(Client* client);
}  // namespace redis_simple::commands::session

namespace redis_simple::commands::transaction {
void HandleDiscard(Client* client);
void HandleExec(Client* client);
void HandleMulti(Client* client);
void HandleUnwatch(Client* client);
void HandleWatch(Client* client);
}  // namespace redis_simple::commands::transaction

namespace redis_simple::commands::persistence {
void HandleBgRewriteAof(Client* client);
void HandleLastSave(Client* client);
void HandleSave(Client* client);
}  // namespace redis_simple::commands::persistence

namespace redis_simple::commands::server_info {
void HandleInfo(Client* client);
}  // namespace redis_simple::commands::server_info

namespace redis_simple::commands::strings {
void HandleAppend(Client* client);
void HandleDecr(Client* client);
void HandleDecrBy(Client* client);
void HandleGet(Client* client);
void HandleIncr(Client* client);
void HandleIncrBy(Client* client);
void HandleMGet(Client* client);
void HandleMSet(Client* client);
void HandleSet(Client* client);
void HandleStrLen(Client* client);
}  // namespace redis_simple::commands::strings

namespace redis_simple::commands::lists {
void HandleLPush(Client* client);
void HandleRPush(Client* client);
void HandleLPop(Client* client);
void HandleRPop(Client* client);
void HandleLLen(Client* client);
void HandleLRange(Client* client);
void HandleLIndex(Client* client);
void HandleLSet(Client* client);
void HandleLRem(Client* client);
void HandleLTrim(Client* client);
}  // namespace redis_simple::commands::lists

namespace redis_simple::commands::sets {
void HandleSAdd(Client* client);
void HandleSCard(Client* client);
void HandleSDiff(Client* client);
void HandleSIsMember(Client* client);
void HandleSInter(Client* client);
void HandleSMembers(Client* client);
void HandleSRem(Client* client);
void HandleSScan(Client* client);
void HandleSUnion(Client* client);
}  // namespace redis_simple::commands::sets

namespace redis_simple::commands::zsets {
void HandleZAdd(Client* client);
void HandleZCard(Client* client);
void HandleZCount(Client* client);
void HandleZRange(Client* client);
void HandleZRangeByScore(Client* client);
void HandleZRevRangeByScore(Client* client);
void HandleZRangeByLex(Client* client);
void HandleZRevRangeByLex(Client* client);
void HandleZLexCount(Client* client);
void HandleZRank(Client* client);
void HandleZRevRank(Client* client);
void HandleZRevRange(Client* client);
void HandleZRem(Client* client);
void HandleZScan(Client* client);
void HandleZScore(Client* client);
}  // namespace redis_simple::commands::zsets

namespace redis_simple::commands::hashes {
void HandleHDel(Client* client);
void HandleHExists(Client* client);
void HandleHGet(Client* client);
void HandleHGetAll(Client* client);
void HandleHIncrBy(Client* client);
void HandleHKeys(Client* client);
void HandleHLen(Client* client);
void HandleHMGet(Client* client);
void HandleHSet(Client* client);
void HandleHMSet(Client* client);
void HandleHScan(Client* client);
void HandleHVals(Client* client);
}  // namespace redis_simple::commands::hashes

#endif  // REDIS_SIMPLE_SERVER_COMMANDS_HANDLERS_H_
