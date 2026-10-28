#include "server/reply.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
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

void AppendBulkString(std::string_view s, std::string* const reply) {
  reply->push_back(kBulkStringPrefix);
  AppendInteger(s.size(), reply);
  reply->append(kCrlf.data(), kCrlf.size())
      .append(s.data(), s.size())
      .append(kCrlf.data(), kCrlf.size());
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
  const std::string value = utils::FloatToString(fl);
  if (protocol == ProtocolVersion::kResp2) {
    return FromBulkString(value);
  }
  std::string reply;
  reply.push_back(kDoublePrefix);
  reply.append(value).append(kCrlf.data(), kCrlf.size());
  return reply;
}

std::string FromError(std::string_view message) {
  std::string reply;
  reply.push_back(kErrorPrefix);
  reply.append(message.data(), message.size())
      .append(kCrlf.data(), kCrlf.size());
  return reply;
}

std::string WrongNumberOfArguments() {
  return std::string(kWrongNumberOfArguments);
}

std::string UnknownCommand(std::string_view command) {
  std::string message = "ERR unknown command '";
  message.append(command.data(), command.size()).push_back('\'');
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
