#include <cstddef>
#include <string_view>
#include <utility>

#include "data_types/zset/zset.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/scan_options.h"
#include "utils/float_utils.h"

namespace redis_simple::commands::zsets {
void HandleZScan(Client* const client) {
  size_t cursor = 0;
  ScanOptions options;
  db::RedisObject* const object =
      StartCollectionScan(client, ScanTarget::kZset, &cursor, &options);
  if (object == nullptr) {
    return;
  }
  zset::ZSet* const zset = object->ZSet();
  const bool skiplist = zset->Encoding() == zset::ZSet::Encoding::kSkiplist;
  ScanReply reply(2 * (skiplist ? options.count : zset->Size()));
  utils::FloatBuffer score_buffer;
  // Redis prints a skiplist's scores with ld2string (17 digits) and a
  // listpack's as the listpack stores them (d2string's text).
  const size_t next_cursor = zset->Scan(
      cursor, options.count,
      [&options, &reply, &score_buffer, skiplist](std::string_view member,
                                                  double score) {
        if (!options.Matches(member)) {
          return;
        }
        reply.Add(member);
        reply.Add(skiplist
                      ? utils::FormatFloatWith17Digits(score, &score_buffer)
                      : utils::FormatFloat(score, &score_buffer));
      });
  std::move(reply).Send(client, next_cursor);
}
}  // namespace redis_simple::commands::zsets
