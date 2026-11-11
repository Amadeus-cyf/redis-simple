#include "utils/lzf.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <vector>

namespace redis_simple::utils {
namespace {
std::string RoundTrip(const std::string& input) {
  // One compressor for every call, as the RDB writer reuses it.
  static LzfCompressor& compressor = *new LzfCompressor;
  // Worst case: one control byte per 32 literal bytes.
  std::vector<char> compressed(input.size() + (input.size() / 32) + 64);
  const size_t size = compressor.Compress(input, compressed);
  if (size == 0) {
    return "<incompressible>";
  }
  std::string output(input.size(), '\0');
  if (!LzfDecompress(std::string_view(compressed.data(), size), output)) {
    return "<failed>";
  }
  return output;
}

TEST(LzfTest, RoundTripsRepetitiveAndRandomData) {
  std::mt19937 rng(7);
  for (size_t size :
       {1, 2, 3, 4, 31, 32, 33, 100, 264, 265, 1000, 9000, 70000}) {
    std::string repetitive;
    for (size_t i = 0; i < size; ++i) {
      repetitive.push_back("abcab"[i % 5]);
    }
    EXPECT_EQ(RoundTrip(repetitive), repetitive) << size;
    std::string random;
    for (size_t i = 0; i < size; ++i) {
      random.push_back(static_cast<char>(rng()));
    }
    EXPECT_EQ(RoundTrip(random), random) << size;
    std::string mixed;
    for (size_t i = 0; i < size; ++i) {
      mixed.push_back(rng() % 4 == 0 ? static_cast<char>(rng()) : 'x');
    }
    EXPECT_EQ(RoundTrip(mixed), mixed) << size;
  }
}

TEST(LzfTest, CompressReportsWhenOutputIsTooSmall) {
  // Redis gives the compressor four bytes less than the input; random data
  // does not fit, so it is stored uncompressed.
  std::mt19937 rng(3);
  std::string random(100, '\0');
  for (char& c : random) {
    c = static_cast<char>(rng());
  }
  LzfCompressor compressor;
  std::vector<char> output(random.size() - 4);
  EXPECT_EQ(compressor.Compress(random, output), 0);
  const std::string zeros(100, '\0');
  EXPECT_GT(compressor.Compress(zeros, output), 0);
  EXPECT_LT(compressor.Compress(zeros, output), 10);
}

TEST(LzfTest, DecompressRejectsMalformedInput) {
  std::string output(10, '\0');
  // A literal run longer than the input that remains.
  EXPECT_FALSE(LzfDecompress(std::string("\x05"
                                         "abc",
                                         4),
                             output));
  // A back reference before the start of the output.
  EXPECT_FALSE(LzfDecompress(std::string("\x00"
                                         "a"
                                         "\x20\x05",
                                         4),
                             output));
  // A back reference missing its distance byte.
  EXPECT_FALSE(LzfDecompress(std::string("\x00"
                                         "a"
                                         "\x20",
                                         3),
                             output));
  // An extended length missing its length byte.
  EXPECT_FALSE(LzfDecompress(std::string("\x00"
                                         "a"
                                         "\xe0",
                                         3),
                             output));
  // Too little or too much output.
  EXPECT_FALSE(LzfDecompress(std::string("\x00"
                                         "a",
                                         2),
                             output));
  std::string small(1, '\0');
  EXPECT_FALSE(LzfDecompress(std::string("\x01"
                                         "ab",
                                         3),
                             small));
  // "a" then nine copies of it: ten bytes.
  EXPECT_TRUE(LzfDecompress(std::string("\x00"
                                        "a"
                                        "\xe0\x00\x00",
                                        5),
                            output));
  EXPECT_EQ(output, std::string(10, 'a'));
}
}  // namespace
}  // namespace redis_simple::utils
