#include "utils/float_utils.h"

#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
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
  EXPECT_EQ(FloatToString(1e17), "1e+17");
  EXPECT_EQ(FloatToString(1.5), "1.5");
  EXPECT_EQ(FloatToString(-2.25), "-2.25");
}

namespace {
// The general path: shortest round-trip digits, printed in fixed notation for
// exponents from -4 to 16.
std::string ReferenceFormat(double value) {
  std::array<char, 64> buffer{};
  auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  EXPECT_EQ(error, std::errc());
  std::string text(buffer.data(), end);
  const size_t exponent_position = text.find('e');
  if (exponent_position != std::string::npos) {
    const int exponent = std::stoi(text.substr(exponent_position + 1));
    if (exponent >= -4 && exponent <= 16) {
      end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                          std::chars_format::fixed)
                .ptr;
      text.assign(buffer.data(), end);
    }
  }
  return text;
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
    EXPECT_EQ(FloatToString(value), ReferenceFormat(value)) << value;
  }
}

TEST(FloatUtilsTest, PlainDecimalsParseLikeTheGeneralParser) {
  // The plain-decimal fast path must return exactly what std::from_chars
  // does, and leave every other form to it.
  const auto reference = [](std::string_view text, double* value) {
    if (!text.empty() && text.front() == '+') {
      text.remove_prefix(1);
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        *value, std::chars_format::general);
    return !text.empty() && result.ec == std::errc() &&
           result.ptr == text.data() + text.size() && !std::isnan(*value);
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
    const auto point = random() % (digits + 2);
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
  EXPECT_FALSE(ToDouble("0x1p2", &value));
  EXPECT_FALSE(ToDouble("", &value));
}
}  // namespace redis_simple::utils
