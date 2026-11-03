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

template <typename Integer>
void AppendInteger(Integer value, std::string* output) {
  std::array<char, std::numeric_limits<Integer>::digits10 + 3> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  RS_CHECK(result.ec == std::errc());
  output->append(buffer.data(),
                 static_cast<size_t>(result.ptr - buffer.data()));
}

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
  reply.push_back(kInt64Prefix);
  AppendInteger(i64, &reply);
  reply.append(kCrlf.data(), kCrlf.size());
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
  reply->push_back(kArrayPrefix);
  AppendInteger(size, reply);
  reply->append(kCrlf.data(), kCrlf.size());
}

namespace {
// Decimal digits in `value`. Most collection elements are short, so lengths
// below 100 skip the loop.
size_t DecimalDigits(size_t value) {
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
void CopyShort(std::string_view s, char* const out) {
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
}  // namespace

void AppendBulkString(std::string_view s, std::string* const reply) {
  const size_t length = s.size();
  const size_t digits = DecimalDigits(length);
  const size_t old_size = reply->size();
  const size_t new_size = old_size + 1 + digits + 2 + length + 2;
  // Grow geometrically so a reply built element by element stays linear,
  // then write the whole element in place with one size update.
  if (new_size > reply->capacity()) {
    reply->reserve(std::max(new_size, 2 * reply->capacity()));
  }
  reply->resize_and_overwrite(new_size, [&](char* const data, size_t) {
    char* out = data + old_size;
    *out = kBulkStringPrefix;
    // Write the length's digits from the last one back.
    size_t remaining = length;
    for (char* digit = out + digits; digit > out; --digit) {
      *digit = static_cast<char>('0' + (remaining % 10));
      remaining /= 10;
    }
    out += 1 + digits;
    *out++ = '\r';
    *out++ = '\n';
    CopyShort(s, out);
    out += length;
    *out++ = '\r';
    *out = '\n';
    return new_size;
  });
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
  reply.push_back(kMapPrefix);
  AppendInteger(size, &reply);
  reply.append(kCrlf.data(), kCrlf.size());
  return reply;
}

std::string FromSetHeader(size_t size, ProtocolVersion protocol) {
  if (protocol == ProtocolVersion::kResp2) {
    return FromArrayHeader(size);
  }
  std::string reply;
  reply.push_back(kSetPrefix);
  AppendInteger(size, &reply);
  reply.append(kCrlf.data(), kCrlf.size());
  return reply;
}
}  // namespace redis_simple::reply
