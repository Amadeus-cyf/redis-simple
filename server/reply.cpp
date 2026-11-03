#include "server/reply.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "utils/check.h"
#include "utils/float_utils.h"

namespace redis_simple::reply {
constexpr std::string_view kCrlf = "\r\n";
constexpr char kStringPrefix = '+';
constexpr char kBulkStringPrefix = '$';
constexpr char kInt64Prefix = ':';
constexpr char kArrayPrefix = '*';
constexpr char kMapPrefix = '%';
constexpr char kSetPrefix = '~';
constexpr char kDoublePrefix = ',';
constexpr char kErrorPrefix = '-';

namespace {
// Appends `prefix`, the decimal `magnitude` (negated when `negative`), and
// CRLF with one size update, writing the digits in place instead of
// formatting them into a buffer and copying.
void AppendLine(char prefix, bool negative, uint64_t magnitude,
                std::string* const reply) {
  const size_t digits = reply_internal::DecimalDigits(magnitude);
  const size_t old_size = reply->size();
  const size_t new_size =
      old_size + 1 + (negative ? 1 : 0) + digits + kCrlf.size();
  if (new_size > reply->capacity()) {
    reply->reserve(std::max(new_size, 2 * reply->capacity()));
  }
  reply->resize_and_overwrite(new_size, [&](char* const data, size_t) {
    char* out = data + old_size;
    *out++ = prefix;
    if (negative) {
      *out++ = '-';
    }
    uint64_t remaining = magnitude;
    for (char* digit = out + digits; digit > out;) {
      *--digit = static_cast<char>('0' + (remaining % 10));
      remaining /= 10;
    }
    out += digits;
    *out++ = '\r';
    *out = '\n';
    return new_size;
  });
}

void AppendSizeLine(char prefix, size_t size, std::string* const reply) {
  AppendLine(prefix, false, size, reply);
}
}  // namespace

std::string FromString(std::string_view s) {
  std::string reply;
  reply.reserve(1 + s.size() + kCrlf.size());
  reply.push_back(kStringPrefix);
  reply.append(s.data(), s.size()).append(kCrlf.data(), kCrlf.size());
  return reply;
}

std::string FromBulkString(std::string_view s) {
  std::string reply;
  AppendBulkString(s, &reply);
  return reply;
}

std::string FromInt64(int64_t i64) {
  std::string reply;
  // Negate in unsigned arithmetic so INT64_MIN does not overflow.
  const auto magnitude = i64 < 0 ? uint64_t{0} - static_cast<uint64_t>(i64)
                                 : static_cast<uint64_t>(i64);
  AppendLine(kInt64Prefix, i64 < 0, magnitude, &reply);
  return reply;
}

std::string FromArray(const std::vector<std::string>& array) {
  std::string reply = FromArrayHeader(array.size());
  for (const std::string& str : array) {
    RS_CHECK(str.ends_with(kCrlf));
    reply.append(str);
  }
  return reply;
}

std::string FromBulkStringArray(const std::vector<std::string>& values) {
  std::string reply = FromArrayHeader(values.size());
  for (const auto& value : values) {
    AppendBulkString(value, &reply);
  }
  return reply;
}

std::string FromArrayHeader(size_t size) {
  std::string reply;
  AppendArrayHeader(size, &reply);
  return reply;
}

void AppendArrayHeader(size_t size, std::string* const reply) {
  AppendSizeLine(kArrayPrefix, size, reply);
}

size_t EncodeBulkStringHeader(size_t length, char* const buffer,
                              size_t capacity) {
  RS_CHECK(capacity >= kBulkStringHeaderCapacity);
  buffer[0] = kBulkStringPrefix;
  const auto result = std::to_chars(buffer + 1, buffer + capacity, length);
  RS_CHECK(result.ec == std::errc());
  char* end = result.ptr;
  *end++ = '\r';
  *end++ = '\n';
  return static_cast<size_t>(end - buffer);
}

std::string FromFloat(double fl, ProtocolVersion protocol) {
  std::string reply;
  AppendFloat(fl, protocol, &reply);
  return reply;
}

void AppendFloat(double fl, ProtocolVersion protocol,
                 std::string* const reply) {
  utils::FloatBuffer buffer;
  const std::string_view value = utils::FormatFloat(fl, &buffer);
  if (protocol == ProtocolVersion::kResp2) {
    AppendBulkString(value, reply);
    return;
  }
  reply->push_back(kDoublePrefix);
  reply->append(value.data(), value.size()).append(kCrlf.data(), kCrlf.size());
}

std::string FromError(std::string_view message) {
  std::string reply;
  reply.push_back(kErrorPrefix);
  reply.append(message.data(), message.size())
      .append(kCrlf.data(), kCrlf.size());
  return reply;
}

std::string WrongNumberOfArguments(std::string_view command) {
  std::string message = "ERR wrong number of arguments for '";
  std::ranges::transform(command, std::back_inserter(message), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  message.append("' command");
  return FromError(message);
}

std::string UnknownCommand(std::string_view command,
                           std::span<const std::string_view> args) {
  // Matches Redis' "'%.128s'" name and its argument list, which stops once
  // 128 bytes are used and cuts the argument that crosses that limit.
  constexpr size_t kLimit = 128;
  std::string quoted_args;
  for (const std::string_view arg : args) {
    if (quoted_args.size() >= kLimit) {
      break;
    }
    quoted_args.push_back('\'');
    quoted_args.append(arg.substr(0, kLimit - (quoted_args.size() - 1)));
    quoted_args.append("' ");
  }
  std::string message = "ERR unknown command '";
  message.append(command.substr(0, kLimit))
      .append("', with args beginning with: ")
      .append(quoted_args);
  // Like Redis, keep client-supplied newlines from breaking the protocol.
  std::ranges::replace(message, '\r', ' ');
  std::ranges::replace(message, '\n', ' ');
  return FromError(message);
}

std::string SyntaxError() { return std::string(kSyntaxError); }

std::string WrongTypeError() { return std::string(kWrongTypeError); }

std::string Null(ProtocolVersion protocol) {
  return std::string(NullView(protocol));
}

std::string FromMapHeader(size_t size, ProtocolVersion protocol) {
  if (protocol == ProtocolVersion::kResp2) {
    RS_CHECK(size <= std::numeric_limits<size_t>::max() / 2);
    return FromArrayHeader(size * 2);
  }
  std::string reply;
  AppendSizeLine(kMapPrefix, size, &reply);
  return reply;
}

std::string FromSetHeader(size_t size, ProtocolVersion protocol) {
  if (protocol == ProtocolVersion::kResp2) {
    return FromArrayHeader(size);
  }
  std::string reply;
  AppendSizeLine(kSetPrefix, size, &reply);
  return reply;
}
}  // namespace redis_simple::reply
