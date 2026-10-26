#include <cstddef>
#include <cstdint>
#include <utility>

#include "data_types/set/set.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/db/db.h"
#include "server/reply.h"

namespace redis_simple::command::sets {
void HandleSAdd(Client* const client) {
  const auto& args = client->Args();
  auto [object, wrong_type] = MutableLookupTyped(
      client->Db(), args[0], db::RedisObject::ObjectType::kSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (object == nullptr) {
    // LLVM 18 cannot trace unique_ptr ownership through Dict::FindOrInsert.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto created = db::RedisObject::CreateWithSet(set::Set::Create());
    object = created.get();
    client->Db()->SetKey(args[0], std::move(created), 0);
  }
  set::Set* const set = object->Set();
  int64_t added = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    added += set->Add(args[i]) ? 1 : 0;
  }
  if (added > 0) {
    client->MarkModified();
  }
  client->AddReply(reply::FromInt64(added));
}
}  // namespace redis_simple::command::sets
