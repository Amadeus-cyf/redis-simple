#include "utils/float_utils.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace redis_simple::utils {
namespace {
// Formats `fl` as fixed-point text with at most kMaxFastDecimals decimals
// when that text is exactly the shortest round-trip form, which covers
// typical scores such as 1.5 or 99.95 without the general shortest-digit
// search. Returns the length written, or 0 to fall back to it.
constexpr int kMaxFastDecimals = 6;
constexpr std::array<double, kMaxFastDecimals + 1> kPowersOfTen = {
    1, 10, 100, 1000, 10000, 100000, 1000000};

size_t FormatShortDecimal(double fl, char* const out) {
  const double magnitude = std::abs(fl);
  // 2^51 / 10^k bounds both the scaled integer below 2^53 and the double
  // spacing near `fl` below half of 10^-k, so exactly one k-decimal number
  // rounds to `fl`, and it is the one the shortest form would print.
  // Values below 10^-4 print in scientific notation instead.
  constexpr double kTwoPow51 = 2251799813685248.0;
  if (!(magnitude >= 1e-4) || magnitude >= kTwoPow51 / kPowersOfTen.back()) {
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
    const size_t length =
        (fl < 0 ? 1 : 0) + integer_digits + 1 + static_cast<size_t>(decimals);
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
    if (fl < 0) {
      *--cursor = '-';
    }
    return length;
  }
  return 0;
}
}  // namespace

std::string_view FormatFloat(double fl, FloatBuffer* const buffer) {
  char* const begin = buffer->data();
  char* const limit = begin + buffer->size();
  // Every integer of magnitude at most 2^53 is exactly representable, so its
  // shortest round-trip text is its digits: format it as an integer, which
  // is several times cheaper. Negative zero keeps the general path's "-0".
  constexpr double kMaxExactInteger = 9007199254740992.0;
  // The cast is in range below 2^53, and converting back compares the value
  // with its integer part without a libm trunc call.
  if (std::abs(fl) <= kMaxExactInteger &&
      static_cast<double>(static_cast<int64_t>(fl)) == fl &&
      (fl != 0 || !std::signbit(fl))) {
    const auto integer = std::to_chars(begin, limit, static_cast<int64_t>(fl));
    return {begin, integer.ptr};
  }
  if (const size_t length = FormatShortDecimal(fl, begin); length != 0) {
    return {begin, length};
  }
  auto [end, error] = std::to_chars(begin, limit, fl);
  if (error != std::errc()) {
    return {};
  }

  const std::string_view general(begin, static_cast<size_t>(end - begin));
  const size_t exponent_position = general.find('e');
  if (exponent_position != std::string_view::npos) {
    int exponent = 0;
    const std::string_view exponent_text =
        general.substr(exponent_position + 1);
    const char* exponent_start = exponent_text.data();
    if (!exponent_text.empty() && exponent_text.front() == '+') {
      ++exponent_start;
    }
    const auto exponent_result = std::from_chars(
        exponent_start, exponent_text.data() + exponent_text.size(), exponent);
    if (exponent_result.ec == std::errc() && exponent >= -4 && exponent <= 16) {
      const auto fixed =
          std::to_chars(begin, limit, fl, std::chars_format::fixed);
      if (fixed.ec == std::errc()) {
        end = fixed.ptr;
      }
    }
  }
  return {begin, end};
}

std::string FloatToString(double fl) {
  FloatBuffer buffer;
  return std::string(FormatFloat(fl, &buffer));
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
  constexpr std::array<double, kMaxDigits + 1> kPowersOfTen = {
      1e0, 1e1, 1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
      1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
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

bool ToDouble(std::string_view value, double* const result) {
  if (value.empty()) {
    return false;
  }
  double fast = 0;
  if (ParsePlainDecimal(value, &fast)) {
    if (result != nullptr) {
      *result = fast;
    }
    return true;
  }
  const char* begin = value.data();
  const char* const end = begin + value.size();
  if (*begin == '+') {
    ++begin;
  }
  double parsed = 0;
  const auto conversion =
      std::from_chars(begin, end, parsed, std::chars_format::general);
  if (begin == end || conversion.ec != std::errc() || conversion.ptr != end ||
      std::isnan(parsed)) {
    return false;
  }
  if (result != nullptr) {
    *result = parsed;
  }
  return true;
}
}  // namespace redis_simple::utils
