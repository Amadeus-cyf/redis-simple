#pragma once

#include <cstddef>
#include <cstdint>
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
inline constexpr std::string_view kWrongNumberOfArguments =
    "-ERR wrong number of arguments\r\n";
inline constexpr std::string_view kSyntaxError = "-ERR syntax error\r\n";
inline constexpr std::string_view kNoAuthError =
    "-NOAUTH Authentication required.\r\n";
inline constexpr std::string_view kWrongTypeError =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
inline constexpr std::string_view kNullResp2 = "$-1\r\n";
inline constexpr std::string_view kNullResp3 = "_\r\n";
// Room for "$", a 64-bit length, and CRLF.
inline constexpr size_t kBulkStringHeaderCapacity = 32;

constexpr std::string_view NullView(ProtocolVersion protocol) {
  return protocol == ProtocolVersion::kResp2 ? kNullResp2 : kNullResp3;
}

std::string FromString(std::string_view s);
std::string FromBulkString(std::string_view s);
std::string FromInt64(int64_t i64);
std::string FromArray(const std::vector<std::string>& array);
std::string FromBulkStringArray(const std::vector<std::string>& values);
std::string FromArrayHeader(size_t size);
void AppendArrayHeader(size_t size, std::string* reply);
void AppendBulkString(std::string_view s, std::string* reply);
// Writes "$<length>\r\n" to `buffer`, which must hold at least
// kBulkStringHeaderCapacity bytes, and returns the encoded size.
size_t EncodeBulkStringHeader(size_t length, char* buffer, size_t capacity);
std::string FromFloat(double fl, ProtocolVersion protocol);
std::string FromError(std::string_view message);
std::string WrongNumberOfArguments();
std::string UnknownCommand(std::string_view command);
std::string SyntaxError();
std::string WrongTypeError();
std::string Null(ProtocolVersion protocol);
std::string FromMapHeader(size_t size, ProtocolVersion protocol);
std::string FromSetHeader(size_t size, ProtocolVersion protocol);
}  // namespace redis_simple::reply
