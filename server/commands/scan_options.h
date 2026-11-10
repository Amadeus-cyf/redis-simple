#ifndef REDIS_SIMPLE_SERVER_COMMANDS_SCAN_OPTIONS_H_
#define REDIS_SIMPLE_SERVER_COMMANDS_SCAN_OPTIONS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "server/db/redis_object.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple {
class Client;
}

namespace redis_simple::commands {
// What a SCAN-family command iterates, which decides the options it takes:
// TYPE only for SCAN, and NOVALUES only for HSCAN.
enum class ScanTarget : uint8_t {
  kKeyspace,
  kHash,
  kSet,
  kZset,
};

struct ScanOptions {
  static constexpr size_t kDefaultCount = 10;

  // How many elements a step should visit.
  size_t count{kDefaultCount};
  std::optional<std::string_view> pattern;
  // Set when TYPE was given. A name that is not a type matches no key, as in
  // Redis 8, which has not made it an error yet.
  bool filter_type{};
  std::optional<db::RedisObject::ObjectType> type;
  // HSCAN NOVALUES: reply with fields only.
  bool no_values{};

  // Whether a key, member, or field matches MATCH.
  bool Matches(std::string_view element) const {
    return !pattern.has_value() || utils::MatchesGlob(element, *pattern);
  }
};

// What Redis replies for a missing key: cursor 0 and no elements.
inline constexpr std::string_view kEmptyScanReply = "*2\r\n$1\r\n0\r\n*0\r\n";

// Parses a cursor as Redis' parseScanCursorOrReply does (string2ull). On
// failure, replies "invalid cursor" and returns false.
bool ParseScanCursor(Client* client, std::string_view text, size_t* cursor);

// Parses the options from argument `first` on as Redis' scanGenericCommand
// does: in any order, each read only when its value follows it. On failure,
// replies with the error and returns false.
bool ParseScanOptions(Client* client, size_t first, ScanTarget target,
                      ScanOptions* options);

// Starts HSCAN, SSCAN, or ZSCAN on the key in argument 0, in Redis' order:
// the cursor in argument 1, then the key, so a missing key replies with an
// empty scan and another type with WRONGTYPE before any option is checked,
// then the options. Returns the collection, or nullptr after replying. The
// object is mutable because scanning a dict pauses its rehashing.
db::RedisObject* StartCollectionScan(Client* client, ScanTarget target,
                                     size_t* cursor, ScanOptions* options);

// Builds a SCAN-family reply, the next cursor and then the elements, which
// are encoded as they are added. Send queues the header and the elements as
// separate buffers, so the elements are not copied.
class ScanReply {
 public:
  // `expected` sizes the first allocation.
  explicit ScanReply(size_t expected);
  void Add(std::string_view element) {
    reply::AppendBulkString(element, &elements_);
    ++count_;
  }
  void Send(Client* client, size_t cursor) &&;

 private:
  std::string elements_;
  size_t count_{};
};
}  // namespace redis_simple::commands

#endif  // REDIS_SIMPLE_SERVER_COMMANDS_SCAN_OPTIONS_H_
