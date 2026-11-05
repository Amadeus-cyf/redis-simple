#ifndef REDIS_SIMPLE_SERVER_REPLY_H_
#define REDIS_SIMPLE_SERVER_REPLY_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::reply {
enum class ProtocolVersion {
  kResp2 = 2,
  kResp3 = 3,
};

// Fixed replies, for Client::AddReply(std::string_view) without allocating.
inline constexpr std::string_view kOk = "+OK\r\n";
inline constexpr std::string_view kSyntaxError = "-ERR syntax error\r\n";
inline constexpr std::string_view kNotAnIntegerError =
    "-ERR value is not an integer or out of range\r\n";
inline constexpr std::string_view kNotAFloatError =
    "-ERR value is not a valid float\r\n";
inline constexpr std::string_view kMinMaxNotFloatError =
    "-ERR min or max is not a float\r\n";
inline constexpr std::string_view kOverflowError =
    "-ERR increment or decrement would overflow\r\n";
inline constexpr std::string_view kNoAuthError =
    "-NOAUTH Authentication required.\r\n";
inline constexpr std::string_view kWrongTypeError =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
inline constexpr std::string_view kNullResp2 = "$-1\r\n";
inline constexpr std::string_view kNullResp3 = "_\r\n";
inline constexpr std::string_view kNullArrayResp2 = "*-1\r\n";
// Room for "$", a 64-bit length, and CRLF.
inline constexpr size_t kBulkStringHeaderCapacity = 32;

constexpr std::string_view NullView(ProtocolVersion protocol) {
  return protocol == ProtocolVersion::kResp2 ? kNullResp2 : kNullResp3;
}

// RESP3 has a single null type; RESP2 distinguishes null arrays.
constexpr std::string_view NullArrayView(ProtocolVersion protocol) {
  return protocol == ProtocolVersion::kResp2 ? kNullArrayResp2 : kNullResp3;
}

std::string FromString(std::string_view s);
std::string FromBulkString(std::string_view s);
std::string FromInt64(int64_t i64);
std::string FromArray(const std::vector<std::string>& array);
std::string FromBulkStringArray(const std::vector<std::string>& values);
std::string FromArrayHeader(size_t size);
void AppendArrayHeader(size_t size, std::string* reply);
namespace reply_internal {
// Decimal digits in `value`. Most collection elements are short, so lengths
// below 100 skip the loop.
inline size_t DecimalDigits(uint64_t value) {
  if (value < 10) {
    return 1;
  }
  if (value < 100) {
    return 2;
  }
  size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

// Copies `s` to `out`. Short values, the common case in collection replies,
// use fixed-size overlapping copies that compile to a few moves instead of a
// memcpy call.
inline void CopyShort(std::string_view s, char* const out) {
  const size_t length = s.size();
  const char* const in = s.data();
  if (length >= 8 && length <= 16) {
    std::memcpy(out, in, 8);
    std::memcpy(out + length - 8, in + length - 8, 8);
  } else if (length >= 4 && length < 8) {
    std::memcpy(out, in, 4);
    std::memcpy(out + length - 4, in + length - 4, 4);
  } else if (length > 16) {
    std::memcpy(out, in, length);
  } else {
    for (size_t i = 0; i < length; ++i) {
      out[i] = in[i];
    }
  }
}
}  // namespace reply_internal

// Appends `s` as a bulk string. Inline because collection replies call it
// once per element.
inline void AppendBulkString(std::string_view s, std::string* const reply) {
  const size_t length = s.size();
  const size_t digits = reply_internal::DecimalDigits(length);
  const size_t old_size = reply->size();
  const size_t new_size = old_size + 1 + digits + 2 + length + 2;
  // Grow geometrically so a reply built element by element stays linear,
  // then write the whole element in place with one size update.
  if (new_size > reply->capacity()) {
    reply->reserve(std::max(new_size, 2 * reply->capacity()));
  }
  reply->resize_and_overwrite(new_size, [&](char* const data, size_t) {
    char* out = data + old_size;
    *out = '$';
    // Write the length's digits from the last one back.
    size_t remaining = length;
    for (char* digit = out + digits; digit > out; --digit) {
      *digit = static_cast<char>('0' + (remaining % 10));
      remaining /= 10;
    }
    out += 1 + digits;
    *out++ = '\r';
    *out++ = '\n';
    reply_internal::CopyShort(s, out);
    out += length;
    *out++ = '\r';
    *out = '\n';
    return new_size;
  });
}
// Writes "$<length>\r\n" to `buffer`, which must hold at least
// kBulkStringHeaderCapacity bytes, and returns the encoded size.
size_t EncodeBulkStringHeader(size_t length, char* buffer, size_t capacity);
std::string FromFloat(double value, ProtocolVersion protocol);
// Appends `value` as a RESP2 bulk string or a RESP3 double.
void AppendFloat(double value, ProtocolVersion protocol, std::string* reply);
std::string FromError(std::string_view message);
// Redis' arity error, which names the command in lower case.
std::string WrongNumberOfArguments(std::string_view command);
// Redis' unknown-command error, which quotes the name and the start of its
// arguments, each cut to fit 128 bytes.
std::string UnknownCommand(std::string_view command,
                           std::span<const std::string_view> args);
std::string SyntaxError();
std::string WrongTypeError();
std::string Null(ProtocolVersion protocol);
std::string FromMapHeader(size_t size, ProtocolVersion protocol);
std::string FromSetHeader(size_t size, ProtocolVersion protocol);
}  // namespace redis_simple::reply

#endif  // REDIS_SIMPLE_SERVER_REPLY_H_
