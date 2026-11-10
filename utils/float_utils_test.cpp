#include "utils/float_utils.h"

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace redis_simple::utils {
TEST(FloatUtilsTest, FormatsCommonValuesConcise) {
  EXPECT_EQ(FloatToString(0.0001), "0.0001");
  EXPECT_EQ(FloatToString(1.5), "1.5");
  EXPECT_EQ(FloatToString(99999.25), "99999.25");
  EXPECT_EQ(FloatToString(100000.0), "100000");
}

TEST(FloatUtilsTest, PreservesDoubleValuesExactly) {
  const std::array values = {
      0.00001,
      1.0000234,
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::max(),
      std::nextafter(1.0, 2.0),
  };
  for (const double value : values) {
    double parsed = 0;
    const std::string text = FloatToString(value);
    ASSERT_TRUE(ToDouble(text, &parsed)) << text;
    EXPECT_EQ(parsed, value) << text;
  }
}

TEST(FloatUtilsTest, FormatsWholeNumbersAsPlainIntegers) {
  // Every integer of magnitude at most 2^53 is exactly representable, so its
  // shortest round-trip text is just its digits.
  constexpr int64_t kMaxExact = int64_t{1} << 53;
  std::vector<int64_t> values = {0,
                                 1,
                                 -1,
                                 10,
                                 100,
                                 1000000,
                                 123456789,
                                 1000000000000000,
                                 kMaxExact - 1,
                                 kMaxExact,
                                 -kMaxExact};
  // Every power of two in range and its neighbors, then an even stride
  // across the whole range, both with each sign.
  for (int bits = 0; bits <= 53; ++bits) {
    const int64_t power = int64_t{1} << bits;
    for (int64_t delta = -3; delta <= 3; ++delta) {
      const int64_t magnitude = power + delta;
      if (magnitude >= 0 && magnitude <= kMaxExact) {
        values.push_back(magnitude);
        values.push_back(-magnitude);
      }
    }
  }
  constexpr int64_t kStride = kMaxExact / 9973;
  for (int64_t magnitude = 0; magnitude <= kMaxExact - 7;
       magnitude += kStride) {
    values.push_back(magnitude + 7);
    values.push_back(-magnitude - 7);
  }
  for (const int64_t value : values) {
    EXPECT_EQ(FloatToString(static_cast<double>(value)), std::to_string(value))
        << value;
  }
  // Negative zero and values just past the exact range keep their formats.
  EXPECT_EQ(FloatToString(-0.0), "-0");
  EXPECT_EQ(FloatToString(9007199254740994.0), "9007199254740994");
  EXPECT_EQ(FloatToString(72057594037927936.0), "72057594037927936");
  // Redis' double2ll: every integral double up to 2^62 prints in full.
  EXPECT_EQ(FloatToString(1e17), "100000000000000000");
  EXPECT_EQ(FloatToString(4611686018427387904.0), "4611686018427387904");
  EXPECT_EQ(FloatToString(-4611686018427387904.0), "-4611686018427387904");
  EXPECT_EQ(FloatToString(1.5), "1.5");
  EXPECT_EQ(FloatToString(-2.25), "-2.25");
}

TEST(FloatUtilsTest, FormatsLikeRedisFpconv) {
  // Outputs of Redis 8.10's fpconv_dtoa (deps/fpconv), run on these inputs.
  const std::vector<std::pair<double, std::string_view>> cases = {
      {694867.4738744653, "6.948674738744653e+5"},
      {1e-05, "0.00001"},
      {1e-07, "1e-7"},
      {0.1, "0.1"},
      {0.0001234567, "1.234567e-4"},
      {-23141.51913071, "-2.314151913071e+4"},
      {9.999e-05, "9.999e-5"},
      {1.5e-07, "1.5e-7"},
      {1e+21, "1e+21"},
      {1.5e+308, "1.5e+308"},
      {5e-324, "5e-324"},
      {123456789.123, "123456789.123"},
      {1234567.0001, "1234567.0001"},
      {0.000123, "0.000123"},
      {9.223372036854776e+18, "9223372036854776000"},
      {1.2345678901234567e+19, "12345678901234567000"},
      {0.30000000000000004, "0.30000000000000004"},
      {0.3333333333333333, "0.3333333333333333"},
      {-2.5e-06, "-2.5e-6"},
      {1e+22, "1e+22"},
      {12345.678901, "12345.678901"},
      {1.7976931348623157e+308, "1.7976931348623157e+308"},
  };
  for (const auto& [value, expected] : cases) {
    EXPECT_EQ(FloatToString(value), expected) << value;
  }
  EXPECT_EQ(FloatToString(std::numeric_limits<double>::infinity()), "inf");
  EXPECT_EQ(FloatToString(-std::numeric_limits<double>::infinity()), "-inf");
  EXPECT_EQ(FloatToString(std::numeric_limits<double>::quiet_NaN()), "nan");
}

namespace {
// fpconv_dtoa's layout of the shortest round-trip digits, written
// independently of FormatFloat for the cross-check below.
std::string ReferenceFormat(double value) {
  std::array<char, 64> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                    std::abs(value), std::chars_format::scientific);
  EXPECT_EQ(error, std::errc());
  const std::string text(buffer.data(), end);
  const size_t e = text.find('e');
  std::string digits = text.substr(0, e);
  std::erase(digits, '.');
  const int exponent = std::stoi(text.substr(e + 1));
  const auto n = static_cast<int>(digits.size());
  const int k = exponent - (n - 1);
  std::string out = value < 0 ? "-" : "";
  if (k >= 0 && std::abs(exponent) < n + 7) {
    return out + digits + std::string(static_cast<size_t>(k), '0');
  }
  if (k < 0 && (k > -7 || std::abs(exponent) < 4)) {
    const int integer_digits = n + k;
    if (integer_digits <= 0) {
      return out + "0." +
             std::string(static_cast<size_t>(-integer_digits), '0') + digits;
    }
    const auto split = static_cast<size_t>(integer_digits);
    return out + digits.substr(0, split) + "." + digits.substr(split);
  }
  out += digits.substr(0, 1);
  if (n > 1) {
    out += "." + digits.substr(1);
  }
  return out + "e" + (exponent < 0 ? "-" : "+") +
         std::to_string(std::abs(exponent));
}
}  // namespace

TEST(FloatUtilsTest, ShortDecimalsMatchShortestRoundTripText) {
  std::vector<double> values = {0.0001,
                                0.00011,
                                0.1,
                                0.25,
                                1.5,
                                -2.25,
                                99.95,
                                0.3,
                                1e-4,
                                9.999e-5,
                                0.1 + 0.2,
                                1.0 / 3,
                                2251799813.6855,
                                2251799813.685248,
                                2251799813.68,
                                123456.789012,
                                -0.000123,
                                0.0001234567};
  // A fixed linear congruential sequence keeps the cases reproducible.
  uint64_t state = 11;
  const auto random = [&state] {
    state = (state * 6364136223846793005U) + 1442695040888963407U;
    return state >> 11;
  };
  for (int i = 0; i < 300'000; ++i) {
    // Decimals with up to 8 places across magnitudes, plus raw bit patterns.
    const auto places = static_cast<int>(random() % 9);
    const auto mantissa = static_cast<double>(random() % 10'000'000'000'000U);
    const auto shift = static_cast<int>(random() % 16);
    double value = mantissa / std::pow(10.0, places + shift);
    if (random() % 2 == 0) {
      value = -value;
    }
    values.push_back(value);
    values.push_back(std::nextafter(value, 0.0));
  }
  for (const double value : values) {
    // Integers up to 2^62 print in full, which the reference also does below
    // 10^16; the fast path must agree with the layout everywhere else.
    if (value != 0) {
      EXPECT_EQ(FloatToString(value), ReferenceFormat(value)) << value;
    }
  }
}

TEST(FloatUtilsTest, PlainDecimalsParseLikeTheGeneralParser) {
  // The plain-decimal fast path must return exactly what Redis' string2d
  // (strtod with its checks) does, and leave every other form to it.
  const auto reference = [](std::string_view text, double* value) {
    if (text.empty() ||
        std::isspace(static_cast<unsigned char>(text[0])) != 0) {
      return false;
    }
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    *value = std::strtod(copy.c_str(), &end);
    const bool range_error =
        errno == ERANGE && (std::isinf(*value) || *value == 0);
    return end == copy.c_str() + copy.size() && !std::isnan(*value) &&
           !range_error;
  };
  std::vector<std::string> inputs = {"0",
                                     "-0",
                                     "+0",
                                     "1",
                                     "-1",
                                     "0.5",
                                     "-0.5",
                                     "+12.25",
                                     "007.50",
                                     "99.95",
                                     "5.",
                                     ".5",
                                     "-.5",
                                     "+-5",
                                     "-+5",
                                     "-",
                                     "+",
                                     "1e3",
                                     "1.5e-3",
                                     "inf",
                                     "-inf",
                                     "nan",
                                     "1..2",
                                     "1.2.3",
                                     "1 ",
                                     " 1",
                                     "0x10",
                                     "1_0",
                                     "123456789012345",
                                     "1234567890123456",
                                     "0.000000000000001",
                                     "9007199254740993",
                                     "999999999999999.9",
                                     "3.14159265358979"};
  // A fixed linear congruential sequence keeps the cases reproducible.
  uint64_t state = 5;
  const auto random = [&state] {
    state = (state * 6364136223846793005U) + 1442695040888963407U;
    return state >> 11;
  };
  for (int i = 0; i < 200'000; ++i) {
    const auto digits = 1 + static_cast<size_t>(random() % 17);
    std::string text = random() % 3 == 0 ? "-" : "";
    for (size_t d = 0; d < digits; ++d) {
      text.push_back(static_cast<char>('0' + (random() % 10)));
    }
    const size_t point = random() % (digits + 2);
    if (point > 0 && point < digits) {
      text.insert(text.size() - point, ".");
    }
    inputs.push_back(text);
  }
  for (const std::string& input : inputs) {
    double expected = 0;
    double actual = 0;
    const bool expected_ok = reference(input, &expected);
    ASSERT_EQ(ToDouble(input, &actual), expected_ok) << input;
    if (expected_ok) {
      EXPECT_EQ(std::signbit(actual), std::signbit(expected)) << input;
      EXPECT_TRUE(actual == expected ||
                  (std::isinf(actual) && std::isinf(expected)))
          << input;
    }
  }
}

TEST(FloatUtilsTest, FormatFloatWritesIntoTheBuffer) {
  FloatBuffer buffer;
  const std::string_view text = FormatFloat(-12.75, &buffer);
  EXPECT_EQ(text, "-12.75");
  EXPECT_EQ(text.data(), buffer.data());
}

TEST(FloatUtilsTest, ParsesOnlyCompleteFiniteOrInfiniteNumbers) {
  double value = 0;
  EXPECT_TRUE(ToDouble("-1.25e+3", &value));
  EXPECT_EQ(value, -1250);
  EXPECT_TRUE(ToDouble("+1.5", &value));
  EXPECT_EQ(value, 1.5);
  EXPECT_TRUE(ToDouble("+inf", &value));
  EXPECT_TRUE(std::isinf(value));
  EXPECT_FALSE(ToDouble("nan", &value));
  EXPECT_FALSE(ToDouble("1.5junk", &value));
  EXPECT_FALSE(ToDouble(" 1.5", &value));
  EXPECT_FALSE(ToDouble("1.5 ", &value));
  EXPECT_FALSE(ToDouble("", &value));
  // strtod forms Redis accepts, and the overflow it does not.
  EXPECT_TRUE(ToDouble("0x10", &value));
  EXPECT_EQ(value, 16);
  EXPECT_TRUE(ToDouble("0x1p2", &value));
  EXPECT_EQ(value, 4);
  EXPECT_TRUE(ToDouble("-Infinity", &value));
  EXPECT_TRUE(std::isinf(value) && value < 0);
  EXPECT_FALSE(ToDouble("1e400", &value));
  EXPECT_FALSE(ToDouble("1e-400", &value));
  EXPECT_FALSE(ToDouble(std::string(200, '1') + "x", &value));
  EXPECT_TRUE(ToDouble(std::string(200, '1'), &value));
}

TEST(FloatUtilsTest, ParsesRangeBoundsLikeZslParseRange) {
  double value = -1;
  EXPECT_TRUE(ToRangeDouble("", &value));
  EXPECT_EQ(value, 0);
  EXPECT_TRUE(ToRangeDouble(" 2.5", &value));
  EXPECT_EQ(value, 2.5);
  EXPECT_TRUE(ToRangeDouble("1e400", &value));
  EXPECT_TRUE(std::isinf(value));
  EXPECT_TRUE(ToRangeDouble("-inf", &value));
  EXPECT_FALSE(ToRangeDouble("nan", &value));
  EXPECT_FALSE(ToRangeDouble("1 ", &value));
  EXPECT_FALSE(ToRangeDouble("abc", &value));
}

TEST(FloatUtilsTest, FormatsWith17DigitsLikeRedisLd2string) {
  const auto format = [](double value) {
    FloatBuffer buffer;
    return std::string(FormatFloatWith17Digits(value, &buffer));
  };
  EXPECT_EQ(format(1.1), "1.1000000000000001");
  EXPECT_EQ(format(0.1 + 0.2), "0.30000000000000004");
  EXPECT_EQ(format(2), "2");
  EXPECT_EQ(format(-0.5), "-0.5");
  EXPECT_EQ(format(0), "0");
  EXPECT_EQ(format(-0.0), "-0");
  EXPECT_EQ(format(1e20), "1e+20");
  EXPECT_EQ(format(1e-7), "9.9999999999999995e-08");
  EXPECT_EQ(format(std::numeric_limits<double>::infinity()), "inf");
  EXPECT_EQ(format(-std::numeric_limits<double>::infinity()), "-inf");
  EXPECT_EQ(format(std::numeric_limits<double>::quiet_NaN()), "nan");
}
}  // namespace redis_simple::utils
