#include "utils/siphash.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace redis_simple::utils {
namespace {
SipHashKey SequentialKey() {
  SipHashKey key{};
  for (size_t index = 0; index < key.size(); ++index) {
    key[index] = static_cast<uint8_t>(index);
  }
  return key;
}

std::string SequentialMessage(size_t length) {
  std::string message(length, '\0');
  for (size_t index = 0; index < length; ++index) {
    message[index] = static_cast<char>(index);
  }
  return message;
}
}  // namespace

TEST(SipHashTest, MatchesReferenceVectors) {
  // Vectors from the SipHash paper and reference implementation (SipHash-2-4,
  // key 00..0f, message 00..length-1).
  const SipHashKey key = SequentialKey();
  EXPECT_EQ((SipHash<2, 4>(SequentialMessage(0), key)), 0x726fdb47dd0e0e31ULL);
  EXPECT_EQ((SipHash<2, 4>(SequentialMessage(1), key)), 0x74f839c593dc67fdULL);
  EXPECT_EQ((SipHash<2, 4>(SequentialMessage(15), key)), 0xa129ca6149be45e5ULL);
}

TEST(SipHashTest, TailLoadsMatchTheByteLoop) {
  // Every tail length, with distinct bytes so a misplaced byte shows.
  for (size_t size = 0; size < 8; ++size) {
    std::array<uint8_t, 8> bytes{};
    uint64_t expected = 0;
    for (size_t index = 0; index < size; ++index) {
      bytes[index] = static_cast<uint8_t>(0xa1 + (index * 0x13));
      expected |= static_cast<uint64_t>(bytes[index]) << (8 * index);
    }
    EXPECT_EQ(siphash_internal::LoadTail(
                  std::span<const uint8_t>(bytes.data(), size)),
              expected)
        << size;
  }
}

TEST(SipHashTest, DependsOnKey) {
  SipHashKey key = SequentialKey();
  const uint64_t original = SipHash<1, 2>("redis", key);
  key[0] ^= 1;
  EXPECT_NE((SipHash<1, 2>("redis", key)), original);
}

TEST(SipHashTest, SeedIsStableWithinProcess) {
  EXPECT_EQ(&HashSeed(), &HashSeed());
  EXPECT_EQ(SeededHash("key"), SeededHash(std::string("key")));
  EXPECT_NE(SeededHash("key1"), SeededHash("key2"));
}
}  // namespace redis_simple::utils
