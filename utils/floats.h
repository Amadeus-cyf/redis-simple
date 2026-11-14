#ifndef REDIS_SIMPLE_UTILS_FLOATS_H_
#define REDIS_SIMPLE_UTILS_FLOATS_H_

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::utils {
// Holds any double's text from FormatFloat.
using FloatBuffer = std::array<char, 64>;
// Writes `value` into `buffer` as Redis 8 replies with it (d2string): "nan",
// "inf", "-inf", "0", and "-0"; integers up to 2^62 in full; anything else
// as the shortest digits that parse back, in fpconv_dtoa's layout, such as
// "0.00001", "1e-7", or "6.948674738744653e+5". Returns a view of the text,
// empty if formatting fails.
std::string_view FormatFloat(double value, FloatBuffer* buffer);
// Writes `value` as Redis' ld2string(LD_STR_AUTO) does, which ZSCAN uses
// for the scores of a skiplist-encoded sorted set: printf's "%.17Lg", so
// 17 significant digits ("1.1000000000000001"), with "inf", "-inf", and
// "nan" spelled out. Returns a view of the text, empty if formatting fails.
std::string_view FormatFloatWith17Digits(double value, FloatBuffer* buffer);
// Convert floating number to string.
std::string FloatToString(double value);
// Parses `value` as Redis' string2d does, for scores and other number
// arguments: strtod over the whole text, so "inf" and hex work, but no
// leading space, NaN, or overflow.
std::optional<double> ToDouble(std::string_view value);
// Parses a score range bound as Redis' zslParseRange does: strtod over the
// whole text, rejecting only NaN, so leading spaces, "" (0), and overflow
// (an infinity) are accepted.
std::optional<double> ToRangeDouble(std::string_view value);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_FLOATS_H_
