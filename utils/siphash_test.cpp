#include "utils/siphash.h"

#include <gtest/gtest.h>

#include <cstdint>
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
  // key 00..0f, message 00..len-1).
  const SipHashKey key = SequentialKey();
  EXPECT_EQ((SipHash<2, 4>(SequentialMessage(0), key)), 0x726fdb47dd0e0e31ULL);
  EXPECT_EQ((SipHash<2, 4>(SequentialMessage(1), key)), 0x74f839c593dc67fdULL);
  EXPECT_EQ((SipHash<2, 4>(SequentialMessage(15), key)), 0xa129ca6149be45e5ULL);
}

TEST(SipHashTest, DependsOnKey) {
  SipHashKey key = SequentialKey();
  const uint64_t original = SipHash<1, 3>("redis", key);
  key[0] ^= 1;
  EXPECT_NE((SipHash<1, 3>("redis", key)), original);
}

TEST(SipHashTest, SeedIsStableWithinProcess) {
  EXPECT_EQ(&HashSeed(), &HashSeed());
  EXPECT_EQ(SeededHash("key"), SeededHash(std::string("key")));
  EXPECT_NE(SeededHash("key1"), SeededHash("key2"));
}
}  // namespace redis_simple::utils
