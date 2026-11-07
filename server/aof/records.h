#ifndef REDIS_SIMPLE_SERVER_AOF_RECORDS_H_
#define REDIS_SIMPLE_SERVER_AOF_RECORDS_H_

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::db {
class RedisDb;
}

namespace redis_simple::aof {
// AOF records are commands in RESP, as a client would send them.

inline constexpr std::string_view kMultiRecord = "*1\r\n$5\r\nMULTI\r\n";
inline constexpr std::string_view kExecRecord = "*1\r\n$4\r\nEXEC\r\n";
// This server has one database. Redis selects it before the first command of
// each file and of a snapshot, and its AOFs load here only if they use no
// other.
inline constexpr std::string_view kSelectRecord =
    "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n";

int64_t UnixSeconds();
// Appends a "#TS:<unix time>" annotation, which loading skips.
void AppendTimestamp(int64_t seconds, std::string* output);

// Appends the record for a successful mutation. Commands whose effect depends
// on the time are recorded by their result, so replay reproduces it: SET
// becomes SET plus an absolute PEXPIREAT, and EXPIRE-family commands become
// PEXPIREAT, or DEL if the key expired. Other commands are recorded as given.
// Returns false, leaving `output` partly written, if the record cannot be
// encoded.
bool EncodeMutation(std::string_view command,
                    const std::vector<std::string_view>& args, db::RedisDb* db,
                    std::string* output);

// Appends a command made of `elements`.
bool AppendFixedCommand(std::initializer_list<std::string_view> elements,
                        std::string* output);
// Appends "PEXPIREAT <key> <expire>".
bool AppendExpireAt(std::string_view key, int64_t expire, std::string* output);

// The encoded size of a bulk string, or SIZE_MAX if it overflows.
size_t RespBulkSize(std::string_view value);
// The encoded size of an array header for `element_count` elements.
size_t RespArrayHeaderSize(size_t element_count);
// Adds `value` to `*total`; false, leaving it unchanged, on overflow.
bool AddSize(size_t value, size_t* total);
}  // namespace redis_simple::aof

#endif  // REDIS_SIMPLE_SERVER_AOF_RECORDS_H_
