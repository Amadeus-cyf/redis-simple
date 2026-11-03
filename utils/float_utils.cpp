#include "utils/float_utils.h"

#include <algorithm>
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
    const double scaled = std::round(magnitude * scale);
    // Both operands are exact, so the quotient is the correctly rounded value
    // of the decimal, which is also what parsing that decimal yields.
    if (scaled / scale != magnitude) {
      continue;
    }
    std::array<char, 24> digits{};
    const auto [digits_end, error] =
        std::to_chars(digits.data(), digits.data() + digits.size(),
                      static_cast<int64_t>(scaled));
    if (error != std::errc()) {
      return 0;
    }
    const auto digit_count = static_cast<size_t>(digits_end - digits.data());
    const auto decimal_count = static_cast<size_t>(decimals);
    char* cursor = out;
    if (fl < 0) {
      *cursor++ = '-';
    }
    if (digit_count <= decimal_count) {
      // 0.00ddd: pad with zeros after the point.
      *cursor++ = '0';
      *cursor++ = '.';
      cursor = std::fill_n(cursor, decimal_count - digit_count, '0');
      cursor = std::copy(digits.data(), digits_end, cursor);
    } else {
      char* const point = digits_end - decimal_count;
      cursor = std::copy(digits.data(), point, cursor);
      *cursor++ = '.';
      cursor = std::copy(point, digits_end, cursor);
    }
    return static_cast<size_t>(cursor - out);
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
  if (std::abs(fl) <= kMaxExactInteger && std::trunc(fl) == fl &&
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

bool ToDouble(std::string_view value, double* const result) {
  if (value.empty()) {
    return false;
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
