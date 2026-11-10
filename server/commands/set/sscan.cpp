#include <cstddef>
#include <string_view>
#include <utility>

#include "data_types/set/set.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/scan_options.h"

namespace redis_simple::commands::sets {
void HandleSScan(Client* const client) {
  size_t cursor = 0;
  ScanOptions options;
  db::RedisObject* const object =
      StartCollectionScan(client, ScanTarget::kSet, &cursor, &options);
  if (object == nullptr) {
    return;
  }
  set::Set* const set = object->Set();
  const bool dict = set->Encoding() == set::Set::Encoding::kDict;
  ScanReply reply(dict ? options.count : set->Size());
  const auto add = [&options, &reply](std::string_view member) {
    if (options.Matches(member)) {
      reply.Add(member);
    }
  };
  size_t next_cursor = 0;
  if (dict) {
    next_cursor = set->ScanDict(cursor, options.count, add);
  } else {
    // An intset or listpack is small, so Redis returns all of it at once,
    // whatever the cursor and COUNT.
    set->ForEachMember([&add](std::string_view member) {
      add(member);
      return true;
    });
  }
  client->AddReply(std::move(reply).Finish(next_cursor));
}
}  // namespace redis_simple::commands::sets
