#ifndef REDIS_SIMPLE_UTILS_FLOAT_UTILS_H_
#define REDIS_SIMPLE_UTILS_FLOAT_UTILS_H_

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace redis_simple::utils {
// Holds any double's text from FormatFloat.
using FloatBuffer = std::array<char, 64>;
// Writes the shortest text that parses back to `value` into `buffer`, in fixed
// notation for decimal exponents from -4 to 16, and returns a view of it.
// The view is empty if formatting fails.
std::string_view FormatFloat(double value, FloatBuffer* buffer);
// Convert floating number to string.
std::string FloatToString(double value);
// Parse the entire string as a double. NaN and out-of-range values are invalid.
bool ToDouble(std::string_view value, double* result);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_FLOAT_UTILS_H_
