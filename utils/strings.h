#ifndef REDIS_SIMPLE_UTILS_STRINGS_H_
#define REDIS_SIMPLE_UTILS_STRINGS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::utils {
std::vector<std::string> Split(const std::string& text,
                               const std::string& delimiter);
std::vector<std::string_view> SplitView(std::string_view text,
                                        std::string_view delimiter);
// Shift a buffer by offset. If offset >= buffer length, clear the entire
// buffer.
void ShiftCString(char* s, size_t length, size_t offset);
// Turn a string to uppercase.
void ToUppercase(std::string& text);
// Compare ASCII text without allocating normalized copies.
bool EqualsIgnoreCase(std::string_view left, std::string_view right);
// Compare secrets in time that depends only on their lengths, not on where
// they first differ.
bool ConstantTimeEquals(std::string_view left, std::string_view right);
// Match Redis-style glob patterns without allocating temporary strings.
bool MatchesGlob(std::string_view value, std::string_view pattern);
// The prefix of a glob pattern of the form `prefix*` whose prefix has no
// special characters, the usual shape of a SCAN pattern. A value matches
// such a pattern exactly when it starts with the prefix, which is cheaper
// to test than MatchesGlob. Returns nullopt for any other pattern.
std::optional<std::string_view> GlobLiteralPrefix(std::string_view pattern);
// Parses `s` as Redis' string2ll does: the canonical decimal text of an
// int64, with an optional '-' and no '+', spaces, leading zeros, or "-0".
// Only canonical text parses, so compact encodings that store a value as an
// integer reproduce the original bytes.
std::optional<int64_t> ToInt64(std::string_view text);
// Parses `s` as Redis' string2ull does, for SCAN cursors: canonical int64
// text that is not negative, or else anything C's strtoull reads in full
// without overflow (leading spaces, a sign, and leading zeros included). The
// text ends at a NUL byte, as Redis reads it as a C string.
std::optional<uint64_t> ToUint64(std::string_view text);
// Convert an int64 to string and store it in `destination`.
int Int64ToString(std::span<char> destination, int64_t value);
// Convert a uint64 to string and store it in `destination`. Ref to
// the following article.
// https://engineering.fb.com/2013/03/15/developer-tools/three-optimization-tips-for-c/
int Uint64ToString(std::span<char> destination, uint64_t value);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_STRINGS_H_
