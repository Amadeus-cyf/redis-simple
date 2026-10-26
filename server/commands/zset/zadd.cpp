#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/float_utils.h"

namespace redis_simple::command::zsets {
void HandleZAdd(Client* const client) {
  const auto& args = client->Args();
  // Validate every score before mutating anything.
  if (args.size() % 2 == 0) {
    client->AddReply(reply::kSyntaxError);
    return;
  }
  for (size_t i = 1; i < args.size(); i += 2) {
    double score = 0.0;
    if (!utils::ToDouble(args[i], &score)) {
      client->AddReply(reply::kSyntaxError);
      return;
    }
  }

  auto [object, wrong_type] = MutableLookupTyped(
      client->Db(), args[0], db::RedisObject::ObjectType::kZSet);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (object == nullptr) {
    // LLVM 18 cannot trace unique_ptr ownership through Dict::FindOrInsert.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto created = db::RedisObject::CreateWithZSet(zset::ZSet::Create());
    object = created.get();
    client->Db()->SetKey(args[0], std::move(created), 0);
  }
  zset::ZSet* const zset = object->ZSet();
  int64_t added = 0;
  for (size_t i = 1; i < args.size(); i += 2) {
    double score = 0.0;
    utils::ToDouble(args[i], &score);
    added += zset->InsertOrUpdate(args[i + 1], score) ? 1 : 0;
  }
  // Score updates change the set even when nothing new was added.
  client->MarkModified();
  client->AddReply(reply::FromInt64(added));
}
}  // namespace redis_simple::command::zsets
