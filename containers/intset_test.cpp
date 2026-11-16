#include "containers/intset.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string>

namespace redis_simple::containers {
namespace {
std::unique_ptr<Intset> MakeFullIntset() {
  auto intset = std::make_unique<Intset>();
  intset->Add(1);
  intset->Add(5);
  intset->Add(7);
  intset->Add(3);
  intset->Add(2);
  intset->Add(-1);
  intset->Add(INT16_MIN);
  intset->Add(INT16_MAX);
  intset->Add(INT32_MIN);
  intset->Add(INT32_MAX);
  intset->Add(INT64_MIN);
  intset->Add(INT64_MAX);
  return intset;
}

std::unique_ptr<Intset> MakeIntsetAfterRemovals() {
  auto intset = MakeFullIntset();
  intset->Delete(INT16_MAX);
  intset->Delete(INT64_MIN);
  intset->Delete(INT64_MAX);
  return intset;
}
}  // namespace

TEST(IntsetTest, AddAndUpgradeAndGet) {
  auto intset = MakeFullIntset();
  ASSERT_FALSE(intset->Add(1));
  ASSERT_EQ(intset->Size(), 12);
  ASSERT_FALSE(intset->Add(INT64_MIN));
  ASSERT_EQ(intset->Size(), 12);
  ASSERT_FALSE(intset->Add(INT64_MAX));
  ASSERT_EQ(intset->Size(), 12);

  ASSERT_EQ(intset->Get(0), INT64_MIN);
  ASSERT_EQ(intset->Get(1), INT32_MIN);
  ASSERT_EQ(intset->Get(2), INT16_MIN);
  ASSERT_EQ(intset->Get(3), -1);
  ASSERT_EQ(intset->Get(4), 1);
  ASSERT_EQ(intset->Get(5), 2);
  ASSERT_EQ(intset->Get(6), 3);
  ASSERT_EQ(intset->Get(7), 5);
  ASSERT_EQ(intset->Get(8), 7);
  ASSERT_EQ(intset->Get(9), INT16_MAX);
  ASSERT_EQ(intset->Get(10), INT32_MAX);
  ASSERT_EQ(intset->Get(11), INT64_MAX);
}

TEST(IntsetTest, Find) {
  auto intset = MakeFullIntset();
  ASSERT_TRUE(intset->Find(INT64_MIN));
  ASSERT_TRUE(intset->Find(INT64_MAX));
  ASSERT_TRUE(intset->Find(INT16_MIN));
  ASSERT_TRUE(intset->Find(INT16_MAX));
  ASSERT_TRUE(intset->Find(-1));
  ASSERT_TRUE(intset->Find(7));
  ASSERT_FALSE(intset->Find(-7));
  ASSERT_FALSE(intset->Find(0));
  ASSERT_FALSE(intset->Find(INT64_MIN + 1));
  ASSERT_FALSE(intset->Find(INT64_MAX - 1));
  ASSERT_FALSE(intset->Find(INT32_MIN + 1));
  ASSERT_FALSE(intset->Find(INT32_MAX - 1));
}

TEST(IntsetTest, Remove) {
  auto intset = MakeFullIntset();
  ASSERT_TRUE(intset->Delete(INT16_MAX));
  ASSERT_FALSE(intset->Find(INT16_MAX));
  ASSERT_EQ(intset->Size(), 11);
  ASSERT_FALSE(intset->Delete(INT16_MAX));
  ASSERT_EQ(intset->Size(), 11);

  ASSERT_TRUE(intset->Delete(INT64_MIN));
  ASSERT_FALSE(intset->Find(INT64_MIN));
  ASSERT_EQ(intset->Size(), 10);
  ASSERT_FALSE(intset->Delete(INT64_MIN));
  ASSERT_EQ(intset->Size(), 10);

  ASSERT_TRUE(intset->Delete(INT64_MAX));
  ASSERT_FALSE(intset->Find(INT64_MAX));
  ASSERT_EQ(intset->Size(), 9);
  ASSERT_FALSE(intset->Delete(INT64_MAX));
  ASSERT_EQ(intset->Size(), 9);
}

TEST(IntsetTest, Iterator) {
  auto intset = MakeIntsetAfterRemovals();
  auto it = Intset::Iterator(intset.get());
  it.SeekToFirst();
  ASSERT_EQ(it.Value(), INT32_MIN);
  it.Next();
  ASSERT_EQ(it.Value(), INT16_MIN);
  it.Next();
  ASSERT_EQ(it.Value(), -1);
  it.Next();
  ASSERT_EQ(it.Value(), 1);
  it.Next();
  ASSERT_EQ(it.Value(), 2);
  it.Next();
  ASSERT_EQ(it.Value(), 3);
  it.Next();
  ASSERT_EQ(it.Value(), 5);
  it.Next();
  ASSERT_EQ(it.Value(), 7);
  it.SeekToLast();
  ASSERT_EQ(it.Value(), INT32_MAX);
}
// Grows and shrinks a set through many allocation sizes, adding and deleting
// at the front, the middle and the back, and compares it with std::set.
TEST(IntsetTest, MatchesModelWhileGrowingAndShrinking) {
  constexpr int64_t kCount = 600;
  for (const int64_t scale : {int64_t{1}, int64_t{100000}, int64_t{1} << 40}) {
    Intset intset;
    std::set<int64_t> model;
    const auto expect_matches_model = [&intset, &model] {
      ASSERT_EQ(intset.Size(), model.size());
      uint32_t index = 0;
      for (const int64_t value : model) {
        ASSERT_EQ(intset.Get(index), value) << "index " << index;
        ++index;
      }
    };
    // Multiplying by 7 modulo 601, a prime, visits every value once in a
    // scattered order.
    for (int64_t i = 1; i <= kCount; ++i) {
      const int64_t value = (((i * 7) % (kCount + 1)) - (kCount / 2)) * scale;
      ASSERT_TRUE(intset.Add(value));
      ASSERT_FALSE(intset.Add(value));
      model.insert(value);
      expect_matches_model();
    }
    while (!model.empty()) {
      const auto victim = std::next(
          model.begin(), static_cast<std::ptrdiff_t>(model.size() * 2 / 3));
      ASSERT_TRUE(intset.Delete(*victim));
      ASSERT_FALSE(intset.Delete(*victim));
      model.erase(victim);
      expect_matches_model();
    }
    ASSERT_TRUE(intset.Add(scale));
    EXPECT_EQ(intset.Get(0), scale);
  }
}

TEST(IntsetDeathTest, RejectsOutOfRangeIndex) {
  auto intset = MakeFullIntset();
  EXPECT_DEATH(intset->Get(-1), "Check failed");
  EXPECT_DEATH(intset->Get(intset->Size()), "Check failed");
}

TEST(IntsetTest, AppendsRedisBlob) {
  Intset intset;
  for (const int64_t value : {1, 2, 300, -70000}) {
    intset.Add(value);
  }
  std::string blob;
  intset.AppendRedisBlob(&blob);
  // The bytes Redis 8.10's DUMP holds for SADD s 1 2 300 -70000: 32-bit
  // encoding, four values, ascending, little-endian.
  const std::string expected(
      "\x04\0\0\0\x04\0\0\0\x90\xee\xfe\xff\x01\0\0\0\x02\0\0\0\x2c\x01\0\0",
      24);
  EXPECT_EQ(blob, expected);
}
}  // namespace redis_simple::containers
