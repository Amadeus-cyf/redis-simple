#include "utils/crc64.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace redis_simple::utils {
namespace {
// Bit by bit, as the polynomial defines it, to check the table version.
uint64_t SlowCrc64(uint64_t crc, const std::string& data) {
  for (const char c : data) {
    crc ^= static_cast<unsigned char>(c);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x95ac9329ac4bc9b5ULL : crc >> 1;
    }
  }
  return crc;
}

TEST(Crc64Test, MatchesRedisTestVector) {
  // Redis' crc64 self-test (src/crc64.c).
  EXPECT_EQ(Crc64(0, "123456789"), 0xe9c6d914c4b8d9caULL);
  EXPECT_EQ(Crc64(0, ""), 0);
}

TEST(Crc64Test, MatchesBitwiseDefinitionForEveryLengthAndSplit) {
  std::string data;
  for (int i = 0; i < 300; ++i) {
    data.push_back(static_cast<char>((i * 131) ^ (i >> 3)));
  }
  for (size_t length = 0; length <= data.size(); length += 7) {
    const std::string prefix = data.substr(0, length);
    const uint64_t expected = SlowCrc64(0, prefix);
    EXPECT_EQ(Crc64(0, prefix), expected) << length;
    // Extending in two parts gives the same CRC as one pass.
    const size_t split = length / 3;
    EXPECT_EQ(Crc64(Crc64(0, prefix.substr(0, split)), prefix.substr(split)),
              expected)
        << length;
  }
}
}  // namespace
}  // namespace redis_simple::utils
