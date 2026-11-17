#include "utils/strings.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "utils/integers.h"

namespace redis_simple::utils {
namespace {
struct GlobToken {
  size_t next{};
  bool matches{};
};

char FoldAsciiCase(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                      : value;
}

GlobToken MatchCharacterClass(std::string_view pattern, size_t offset,
                              char value) {
  size_t cursor = offset + 1;
  const bool negate = cursor < pattern.size() && pattern[cursor] == '^';
  cursor += negate ? 1 : 0;

  bool matched = false;
  while (cursor < pattern.size() && pattern[cursor] != ']') {
    char range_start = pattern[cursor++];
    if (range_start == '\\' && cursor < pattern.size()) {
      range_start = pattern[cursor++];
    }

    if (cursor + 1 < pattern.size() && pattern[cursor] == '-' &&
        pattern[cursor + 1] != ']') {
      ++cursor;
      char range_end = pattern[cursor++];
      if (range_end == '\\' && cursor < pattern.size()) {
        range_end = pattern[cursor++];
      }
      const auto candidate = static_cast<unsigned char>(value);
      const auto start = static_cast<unsigned char>(range_start);
      const auto end = static_cast<unsigned char>(range_end);
      matched =
          matched || (start <= end ? candidate >= start && candidate <= end
                                   : candidate >= end && candidate <= start);
      continue;
    }
    matched = matched || value == range_start;
  }

  if (cursor == pattern.size()) {
    return GlobToken{offset + 1, value == '['};
  }
  return GlobToken{cursor + 1, negate ? !matched : matched};
}

GlobToken MatchGlobToken(std::string_view pattern, size_t offset, char value) {
  if (pattern[offset] == '?') {
    return GlobToken{offset + 1, true};
  }
  if (pattern[offset] == '[') {
    return MatchCharacterClass(pattern, offset, value);
  }
  if (pattern[offset] == '\\' && offset + 1 < pattern.size()) {
    return GlobToken{offset + 2, pattern[offset + 1] == value};
  }
  return GlobToken{offset + 1, pattern[offset] == value};
}
}  // namespace

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i) {
    if (FoldAsciiCase(left[i]) != FoldAsciiCase(right[i])) {
      return false;
    }
  }
  return true;
}

bool ConstantTimeEquals(std::string_view left, std::string_view right) {
  const size_t length = std::max(left.size(), right.size());
  unsigned char difference = left.size() == right.size() ? 0 : 1;
  for (size_t i = 0; i < length; ++i) {
    const auto left_byte =
        static_cast<unsigned char>(i < left.size() ? left[i] : 0);
    const auto right_byte =
        static_cast<unsigned char>(i < right.size() ? right[i] : 0);
    difference |= static_cast<unsigned char>(left_byte ^ right_byte);
  }
  return difference == 0;
}

bool MatchesGlob(std::string_view value, std::string_view pattern) {
  size_t value_offset = 0;
  size_t pattern_offset = 0;
  size_t retry_pattern = std::string_view::npos;
  size_t retry_value = 0;

  while (value_offset < value.size()) {
    if (pattern_offset < pattern.size() && pattern[pattern_offset] == '*') {
      while (pattern_offset < pattern.size() &&
             pattern[pattern_offset] == '*') {
        ++pattern_offset;
      }
      retry_pattern = pattern_offset;
      retry_value = value_offset;
      continue;
    }

    if (pattern_offset < pattern.size()) {
      const GlobToken token =
          MatchGlobToken(pattern, pattern_offset, value[value_offset]);
      if (token.matches) {
        pattern_offset = token.next;
        ++value_offset;
        continue;
      }
    }

    if (retry_pattern == std::string_view::npos) {
      return false;
    }
    pattern_offset = retry_pattern;
    value_offset = ++retry_value;
  }

  while (pattern_offset < pattern.size() && pattern[pattern_offset] == '*') {
    ++pattern_offset;
  }
  return pattern_offset == pattern.size();
}

std::optional<std::string_view> GlobLiteralPrefix(std::string_view pattern) {
  if (!pattern.ends_with('*')) {
    return std::nullopt;
  }
  const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
  if (prefix.find_first_of("*?[\\") != std::string_view::npos) {
    return std::nullopt;
  }
  return prefix;
}

std::optional<int64_t> ToInt64(std::string_view text) {
  // Redis' string2ll: an optional '-', then digits with no leading zero,
  // so "0" parses and "-0", "01", "+1", and "" do not.
  const bool negative = text.starts_with('-');
  const std::string_view digits = text.substr(negative ? 1 : 0);
  // Up to 19 digits fit in a uint64 without overflow; int64 has no more.
  constexpr size_t kMaxDigits = 19;
  if (digits.empty() || digits.size() > kMaxDigits) {
    return std::nullopt;
  }
  if (digits[0] == '0') {
    return digits.size() == 1 && !negative ? std::optional<int64_t>(0)
                                           : std::nullopt;
  }
  uint64_t magnitude = 0;
  for (const char character : digits) {
    const auto digit = static_cast<uint64_t>(character - '0');
    if (digit > 9) {
      return std::nullopt;
    }
    magnitude = (magnitude * 10) + digit;
  }
  constexpr auto kMaxPositive =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (magnitude > kMaxPositive + (negative ? 1 : 0)) {
    return std::nullopt;
  }
  // Negated as unsigned, so the magnitude of INT64_MIN does not overflow.
  return static_cast<int64_t>(negative ? 0 - magnitude : magnitude);
}

std::optional<uint64_t> ToUint64(std::string_view text) {
  text = text.substr(0, text.find('\0'));
  if (const std::optional<int64_t> signed_value = ToInt64(text);
      signed_value.has_value()) {
    if (*signed_value < 0) {
      return std::nullopt;
    }
    return static_cast<uint64_t>(*signed_value);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  // strtoull needs a terminated string; cursors are short.
  const std::string terminated(text);
  char* end = nullptr;
  errno = 0;
  const uint64_t parsed = std::strtoull(terminated.c_str(), &end, 10);
  if (errno == EINVAL || errno == ERANGE ||
      end != terminated.c_str() + terminated.size()) {
    return std::nullopt;
  }
  return parsed;
}

int Int64ToString(std::span<char> destination, int64_t value) {
  if (value >= 0) {
    return Uint64ToString(destination, static_cast<uint64_t>(value));
  }
  // Negate in unsigned arithmetic so INT64_MIN does not overflow.
  const uint64_t magnitude = 0 - static_cast<uint64_t>(value);
  if (destination.size() < 2) {
    if (!destination.empty()) {
      destination[0] = '\0';
    }
    return 0;
  }
  destination[0] = '-';
  const int length = Uint64ToString(destination.subspan(1), magnitude);
  return length == 0 ? 0 : length + 1;
}

int Uint64ToString(std::span<char> destination, uint64_t value) {
  static constexpr std::string_view kDigits =
      "0001020304050607080910111213141516171819"
      "2021222324252627282930313233343536373839"
      "4041424344454647484950515253545556575859"
      "6061626364656667686970717273747576777879"
      "8081828384858687888990919293949596979899";
  // The digits and the NUL byte must fit.
  const uint32_t length = Digits10(value);
  if (length >= destination.size()) {
    if (!destination.empty()) {
      destination[0] = '\0';
    }
    return 0;
  }
  // Write the NUL byte, then the digits from the last one backward.
  uint32_t next = length - 1;
  destination[next + 1] = '\0';
  while (value >= 100) {
    const int i = static_cast<int>((value % 100) * 2);
    value /= 100;
    destination[next] = kDigits[i + 1];
    destination[next - 1] = kDigits[i];
    next -= 2;
  }
  // Handle the last 1-2 digits.
  if (value < 10) {
    destination[next] = static_cast<char>('0' + static_cast<uint32_t>(value));
  } else {
    const int i = static_cast<int>(static_cast<uint32_t>(value) * 2);
    destination[next] = kDigits[i + 1];
    destination[next - 1] = kDigits[i];
  }
  return static_cast<int>(length);
}
}  // namespace redis_simple::utils
