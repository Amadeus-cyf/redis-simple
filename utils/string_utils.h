#ifndef REDIS_SIMPLE_UTILS_STRING_UTILS_H_
#define REDIS_SIMPLE_UTILS_STRING_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::utils {
std::vector<std::string> Split(const std::string& s,
                               const std::string& delimiter);
std::vector<std::string_view> SplitView(std::string_view s,
                                        std::string_view delimiter);
// Shift a buffer by offset. If offset >= buffer len, clear the entire buffer.
void ShiftCString(char* s, size_t len, size_t offset);
// Turn a string to uppercase.
void ToUppercase(std::string& s);
// Compare ASCII text without allocating normalized copies.
bool EqualsIgnoreCase(std::string_view left, std::string_view right);
// Compare secrets in time that depends only on their lengths, not on where
// they first differ.
bool ConstantTimeEquals(std::string_view left, std::string_view right);
// Match Redis-style glob patterns without allocating temporary strings.
bool MatchesGlob(std::string_view value, std::string_view pattern);
// Return true if the string strictly represents a signed int64: no leading or
// trailing spaces, no extra characters, and no leading zeroes except "0".
bool ToInt64(std::string_view s, int64_t* v);
// Parse an integer only when its text is the canonical decimal representation.
// Compact encodings use this to preserve distinct values such as "+1" and "1".
bool ToCanonicalInt64(std::string_view s, int64_t* v);
// Convert an int64 to string and store it in dst.
int Int64ToString(char* dst, size_t dstlen, int64_t svalue);
// Convert a uint64 to string and store it in dst. Ref to
// the following article.
// https://engineering.fb.com/2013/03/15/developer-tools/three-optimization-tips-for-c/
int Uint64ToString(char* dst, size_t dstlen, uint64_t value);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_STRING_UTILS_H_
