#include "utils/floats.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace redis_simple::utils {
namespace {
// Formats `value` as fixed-point text with at most kMaxFastDecimals decimals
// when that text is exactly the shortest round-trip form, which covers
// typical scores such as 1.5 or 99.95 without the general shortest-digit
// search. Returns the length written, or 0 to fall back to it.
constexpr int kMaxFastDecimals = 6;
// Powers of ten that are exact doubles, shared by the fast formatting and
// parsing paths.
constexpr std::array<double, 16> kPowersOfTen = {
    1e0, 1e1, 1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};

size_t FormatShortDecimal(double value, char* const out) {
  const double magnitude = std::abs(value);
  // 2^51 / 10^k bounds both the scaled integer below 2^53 and the double
  // spacing near `value` below half of 10^-k, so exactly one k-decimal number
  // rounds to `value`, and it is the one the shortest form would print.
  // Values below 10^-4 print in scientific notation instead.
  constexpr double kTwoPow51 = 2251799813685248.0;
  if (!(magnitude >= 1e-4) ||
      magnitude >= kTwoPow51 / kPowersOfTen[kMaxFastDecimals]) {
    return 0;
  }
  for (int decimals = 1; decimals <= kMaxFastDecimals; ++decimals) {
    const double scale = kPowersOfTen[static_cast<size_t>(decimals)];
    // Rounds without a libm call. A wrong candidate cannot slip through:
    // the check below rejects it, and the general path prints the value.
    // NOLINTNEXTLINE(bugprone-incorrect-roundings): verified below.
    const auto scaled = static_cast<uint64_t>((magnitude * scale) + 0.5);
    // Both operands are exact, so the quotient is the correctly rounded value
    // of the decimal, which is also what parsing that decimal yields.
    if (static_cast<double>(scaled) / scale != magnitude) {
      continue;
    }
    // Size the text first, then write it backward straight into `out`: the
    // decimals, the point, then the integer part, which is at least "0".
    uint64_t integer_part = scaled;
    for (int i = 0; i < decimals; ++i) {
      integer_part /= 10;
    }
    size_t integer_digits = 1;
    for (uint64_t rest = integer_part; rest >= 10; rest /= 10) {
      ++integer_digits;
    }
    const size_t length = (value < 0 ? 1 : 0) + integer_digits + 1 +
                          static_cast<size_t>(decimals);
    char* cursor = out + length;
    uint64_t remaining = scaled;
    for (int i = 0; i < decimals; ++i) {
      *--cursor = static_cast<char>('0' + (remaining % 10));
      remaining /= 10;
    }
    *--cursor = '.';
    for (size_t i = 0; i < integer_digits; ++i) {
      *--cursor = static_cast<char>('0' + (remaining % 10));
      remaining /= 10;
    }
    if (value < 0) {
      *--cursor = '-';
    }
    return length;
  }
  return 0;
}
}  // namespace

std::string_view FormatFloat(double value, FloatBuffer* const buffer) {
  // Redis 8's d2string: nan, infinities, and zeros spelled out; integers up
  // to 2^62 as integers; anything else as the shortest digits that parse
  // back, laid out by fpconv_dtoa's rules.
  char* const begin = buffer->data();
  char* const limit = begin + buffer->size();
  const auto literal = [begin](std::string_view text) {
    std::ranges::copy(text, begin);
    return std::string_view(begin, text.size());
  };
  // Redis' double2ll, checked first as the common case: in this range every
  // double converts to int64 exactly, and an integral one prints as an
  // integer. NaN and infinities fail the range check; negative zero is
  // spelled out below.
  constexpr double kMaxIntegral = 4611686018427387904.0;  // 2^62
  if (std::abs(value) <= kMaxIntegral &&
      static_cast<double>(static_cast<int64_t>(value)) == value &&
      (value != 0 || !std::signbit(value))) {
    const auto integer =
        std::to_chars(begin, limit, static_cast<int64_t>(value));
    return {begin, integer.ptr};
  }
  if (std::isnan(value)) {
    return literal("nan");
  }
  if (std::isinf(value)) {
    return literal(value < 0 ? "-inf" : "inf");
  }
  if (value == 0) {
    return literal("-0");
  }
  if (const size_t length = FormatShortDecimal(value, begin); length != 0) {
    return {begin, length};
  }
  // The shortest digits, as "d.ddde±x", then laid out as fpconv_dtoa does.
  std::array<char, 32> scientific{};
  const auto [end, error] =
      std::to_chars(scientific.data(), scientific.data() + scientific.size(),
                    std::abs(value), std::chars_format::scientific);
  if (error != std::errc()) {
    return {};
  }
  const std::string_view text(scientific.data(),
                              static_cast<size_t>(end - scientific.data()));
  const size_t exponent_position = text.find('e');
  std::array<char, 20> digits{};
  size_t count = 0;
  for (const char c : text.substr(0, exponent_position)) {
    if (c != '.') {
      digits[count++] = c;
    }
  }
  int exponent = 0;
  const std::string_view exponent_text = text.substr(exponent_position + 1);
  std::from_chars(exponent_text.data() + (exponent_text.front() == '+' ? 1 : 0),
                  exponent_text.data() + exponent_text.size(), exponent);
  const auto digit_count = static_cast<int>(count);
  // value = digits * 10^k, and `magnitude` is the decimal exponent's size.
  const int k = exponent - (digit_count - 1);
  const int magnitude = std::abs(exponent);
  char* out = begin;
  if (value < 0) {
    *out++ = '-';
  }
  const std::string_view all_digits(digits.data(), count);
  if (k >= 0 && magnitude < digit_count + 7) {
    // An integer beyond 2^62: its digits, then zeros.
    out = std::ranges::copy(all_digits, out).out;
    out = std::fill_n(out, k, '0');
  } else if (k < 0 && (k > -7 || magnitude < 4)) {
    // Plain decimal notation.
    const int integer_digits = digit_count + k;
    if (integer_digits <= 0) {
      *out++ = '0';
      *out++ = '.';
      out = std::fill_n(out, -integer_digits, '0');
      out = std::ranges::copy(all_digits, out).out;
    } else {
      const auto split = static_cast<size_t>(integer_digits);
      out = std::ranges::copy(all_digits.substr(0, split), out).out;
      *out++ = '.';
      out = std::ranges::copy(all_digits.substr(split), out).out;
    }
  } else {
    // Scientific notation, with an unpadded exponent such as "1e-7".
    *out++ = all_digits.front();
    if (count > 1) {
      *out++ = '.';
      out = std::ranges::copy(all_digits.substr(1), out).out;
    }
    *out++ = 'e';
    *out++ = exponent < 0 ? '-' : '+';
    out = std::to_chars(out, limit, magnitude).ptr;
  }
  return {begin, static_cast<size_t>(out - begin)};
}

std::string_view FormatFloatWith17Digits(double value,
                                         FloatBuffer* const buffer) {
  char* const begin = buffer->data();
  const auto literal = [begin](std::string_view text) {
    std::ranges::copy(text, begin);
    return std::string_view(begin, text.size());
  };
  if (std::isnan(value)) {
    return literal("nan");
  }
  if (std::isinf(value)) {
    return literal(value < 0 ? "-inf" : "inf");
  }
  // A double widened to long double has the same value, so printing either
  // to 17 significant digits gives the same text; std::to_chars with a
  // precision prints exactly what printf's "%.17g" would.
  constexpr int kDigits = 17;
  const auto [end, error] = std::to_chars(begin, begin + buffer->size(), value,
                                          std::chars_format::general, kDigits);
  if (error != std::errc()) {
    return {};
  }
  return {begin, end};
}

std::string FloatToString(double value) {
  FloatBuffer buffer;
  return std::string(FormatFloat(value, &buffer));
}

namespace {
// Parses plain decimals such as "12", "-0.5", or "99.95", with at most 15
// significant digits and no exponent, without the general parser. Such a
// number is n / 10^k with n below 2^53 and k at most 15, and both are exact
// doubles, so the correctly rounded quotient is exactly the value
// std::from_chars returns (Clinger's fast path). Returns false to fall back
// to the general parser, never for a valid number it cannot handle.
bool ParsePlainDecimal(std::string_view value, double* const result) {
  constexpr size_t kMaxDigits = 15;
  static_assert(kMaxDigits < kPowersOfTen.size());
  size_t position = 0;
  const bool negative = value[0] == '-';
  if (negative || value[0] == '+') {
    position = 1;
  }
  uint64_t mantissa = 0;
  size_t digits = 0;
  size_t decimals = 0;
  bool seen_point = false;
  bool digit_before_point = false;
  for (; position < value.size(); ++position) {
    const char character = value[position];
    if (character >= '0' && character <= '9') {
      if (++digits > kMaxDigits) {
        return false;
      }
      mantissa = (mantissa * 10) + static_cast<uint64_t>(character - '0');
      decimals += seen_point ? 1 : 0;
      digit_before_point |= !seen_point;
    } else if (character == '.' && !seen_point && digit_before_point) {
      seen_point = true;
    } else {
      return false;
    }
  }
  // Require digits on both sides of a point, leaving forms such as "5." to
  // the general parser.
  if (!digit_before_point || (seen_point && decimals == 0)) {
    return false;
  }
  double parsed = static_cast<double>(mantissa) / kPowersOfTen[decimals];
  *result = negative ? -parsed : parsed;
  return true;
}
}  // namespace

namespace {
// C's strtod over all of `value`, which need not be NUL-terminated. Returns
// false unless it consumes every byte; `*overflow` reports ERANGE.
bool ParseWithStrtod(std::string_view value, double* const result,
                     bool* const overflow) {
  // Long inputs are rare; numbers that need more digits than this to
  // round correctly do not exist.
  constexpr size_t kInlineBytes = 128;
  std::array<char, kInlineBytes> inline_buffer{};
  std::string heap_buffer;
  char* text = inline_buffer.data();
  if (value.size() >= kInlineBytes) {
    heap_buffer.assign(value);
    text = heap_buffer.data();
  } else {
    std::ranges::copy(value, inline_buffer.begin());
  }
  errno = 0;
  char* end = nullptr;
  *result = std::strtod(text, &end);
  *overflow = errno == ERANGE;
  return end == text + value.size();
}
}  // namespace

std::optional<double> ToDouble(std::string_view value) {
  // Redis' string2d: no leading space, the whole text, no NaN, and no
  // overflow or underflow to zero; strtod also takes "inf" and hex.
  if (value.empty() ||
      std::isspace(static_cast<unsigned char>(value[0])) != 0) {
    return std::nullopt;
  }
  double parsed = 0;
  if (ParsePlainDecimal(value, &parsed)) {
    return parsed;
  }
  bool overflow = false;
  if (!ParseWithStrtod(value, &parsed, &overflow) || std::isnan(parsed) ||
      (overflow && (std::isinf(parsed) || parsed == 0))) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<double> ToRangeDouble(std::string_view value) {
  // Redis' zslParseRange: strtod over the whole text, NaN rejected. Unlike
  // ToDouble, it takes leading spaces, an empty text as 0, and overflow as
  // an infinity.
  double parsed = 0;
  bool overflow = false;
  if (!ParseWithStrtod(value, &parsed, &overflow) || std::isnan(parsed)) {
    return std::nullopt;
  }
  return parsed;
}
}  // namespace redis_simple::utils
