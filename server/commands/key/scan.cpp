#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/scan_options.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"

namespace redis_simple::commands::keys {
void HandleScan(Client* const client) {
  size_t cursor = 0;
  ScanOptions options;
  if (!ParseScanCursor(client, client->Args()[0], &cursor) ||
      !ParseScanOptions(client, 1, ScanTarget::kKeyspace, &options)) {
    return;
  }
  db::RedisDb* const redis_db = client->Db();
  ScanReply reply(std::min(options.count, redis_db->KeyCount()));
  // Like Redis, filter the visited keys, so a filtered step may return fewer
  // keys than COUNT, or none, with a nonzero cursor.
  const auto add_key = [&options, &reply](std::string_view key,
                                          const db::RedisObject& object) {
    if ((options.filter_type && options.type != object.Type()) ||
        !options.Matches(key)) {
      return;
    }
    reply.Add(key);
  };
  const size_t next_cursor = redis_db->ScanKeys(cursor, options.count, add_key);
  std::move(reply).Send(client, next_cursor);
}
}  // namespace redis_simple::commands::keys
