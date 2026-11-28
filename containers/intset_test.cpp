#include "containers/intset.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "containers/intset_test_util.h"

namespace redis_simple::containers {
namespace {
using intset_test::Blob;
using intset_test::RawBlob;

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
// Grows and shrinks a set through many allocation sizes, adding in a
// scattered order and deleting from two thirds of the way in, and compares
// it with std::set.
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
  // The bytes Redis 8.12's DUMP holds for SADD s 1 2 300 -70000: 32-bit
  // encoding, four values, ascending, little-endian.
  const std::string expected(
      "\x04\0\0\0\x04\0\0\0\x90\xee\xfe\xff\x01\0\0\0\x02\0\0\0\x2c\x01\0\0",
      24);
  EXPECT_EQ(blob, expected);
}

TEST(IntsetTest, CreatesFromARedisBlob) {
  // The bytes Redis 8.12's DUMP holds for SADD s 1 2 300 -70000.
  const std::string blob(
      "\x04\0\0\0\x04\0\0\0\x90\xee\xfe\xff\x01\0\0\0\x02\0\0\0\x2c\x01\0\0",
      24);
  const std::unique_ptr<Intset> intset = Intset::CreateFromRedisBlob(blob);
  ASSERT_NE(intset, nullptr);
  ASSERT_EQ(intset->Size(), 4);
  EXPECT_EQ(intset->Get(0), -70000);
  EXPECT_EQ(intset->Get(1), 1);
  EXPECT_EQ(intset->Get(2), 2);
  EXPECT_EQ(intset->Get(3), 300);
  EXPECT_TRUE(intset->Find(300));
  EXPECT_FALSE(intset->Find(3));
  std::string written;
  intset->AppendRedisBlob(&written);
  EXPECT_EQ(written, blob);
}

TEST(IntsetTest, RedisBlobsRoundTripAtEachWidth) {
  struct Case {
    size_t width;
    std::vector<int64_t> values;
  };
  const std::vector<Case> cases = {
      {.width = 2, .values = {INT16_MIN, -1, 0, 1, INT16_MAX}},
      {.width = 4, .values = {INT32_MIN, INT16_MIN, 0, INT16_MAX, INT32_MAX}},
      {.width = 8, .values = {INT64_MIN, INT32_MIN, 0, INT32_MAX, INT64_MAX}},
  };
  for (const Case& test : cases) {
    Intset original;
    // Added from both ends, so the blob's order is the set's and not the
    // order of the adds.
    for (size_t i = 0; i < test.values.size(); ++i) {
      const size_t index =
          i % 2 == 0 ? i / 2 : test.values.size() - 1 - (i / 2);
      original.Add(test.values[index]);
    }
    std::string blob;
    original.AppendRedisBlob(&blob);
    ASSERT_EQ(blob.size(), 8 + (test.values.size() * test.width));

    const std::unique_ptr<Intset> copy = Intset::CreateFromRedisBlob(blob);
    ASSERT_NE(copy, nullptr) << test.width;
    ASSERT_EQ(copy->Size(), test.values.size());
    for (uint32_t index = 0; index < copy->Size(); ++index) {
      EXPECT_EQ(copy->Get(index), test.values[index]) << test.width;
    }
    EXPECT_EQ(copy->Min(), test.values.front());
    EXPECT_EQ(copy->Max(), test.values.back());
    std::string written;
    copy->AppendRedisBlob(&written);
    EXPECT_EQ(written, blob) << test.width;
  }
}

// Redis never narrows a set, so a file can hold small values at a wide width.
// The set keeps that width, and so writes the bytes it read.
TEST(IntsetTest, KeepsTheWidthOfABlobWithSmallValues) {
  const std::unique_ptr<Intset> intset =
      Intset::CreateFromRedisBlob(Blob(8, {-3, 1, 2}));
  ASSERT_NE(intset, nullptr);
  EXPECT_TRUE(intset->Find(-3));
  EXPECT_TRUE(intset->Add(0));
  EXPECT_FALSE(intset->Add(2));
  EXPECT_TRUE(intset->Delete(1));
  std::string written;
  intset->AppendRedisBlob(&written);
  EXPECT_EQ(written, Blob(8, {-3, 0, 2}));
}

TEST(IntsetTest, CreatesAnEmptySetFromABlobWithoutValues) {
  for (const uint32_t width : {2U, 4U, 8U}) {
    const std::unique_ptr<Intset> intset =
        Intset::CreateFromRedisBlob(Blob(width, {}));
    ASSERT_NE(intset, nullptr) << width;
    EXPECT_EQ(intset->Size(), 0);
    EXPECT_FALSE(intset->Find(0));
    EXPECT_TRUE(intset->Add(INT32_MAX));
    EXPECT_EQ(intset->Get(0), INT32_MAX);
  }
}

// A set made from a blob has the allocation its length needs, so it grows and
// shrinks from there as one built by adding does. The lengths sit on both
// sides of the sizes at which the allocation changes.
TEST(IntsetTest, GrowsAndShrinksAfterCreationFromABlob) {
  for (const uint32_t width : {2U, 4U, 8U}) {
    for (const int64_t count : {0, 1, 3, 4, 5, 8, 9, 31, 32, 33, 100}) {
      std::vector<int64_t> values;
      values.reserve(static_cast<size_t>(count));
      for (int64_t i = 0; i < count; ++i) {
        values.push_back(i * 3);
      }
      const std::unique_ptr<Intset> intset =
          Intset::CreateFromRedisBlob(Blob(width, values));
      ASSERT_NE(intset, nullptr);
      std::set<int64_t> model(values.begin(), values.end());
      const auto expect_matches_model = [&intset, &model] {
        ASSERT_EQ(intset->Size(), model.size());
        uint32_t index = 0;
        for (const int64_t value : model) {
          ASSERT_EQ(intset->Get(index), value) << "index " << index;
          ++index;
        }
      };
      expect_matches_model();
      // Some of these are already members, the others land before, between
      // and after them.
      for (int64_t i = 0; i < 40; ++i) {
        const int64_t value = (i * 7) - 20;
        ASSERT_EQ(intset->Add(value), model.insert(value).second);
        expect_matches_model();
      }
      while (!model.empty()) {
        const auto victim = std::next(
            model.begin(), static_cast<std::ptrdiff_t>(model.size() / 2));
        ASSERT_TRUE(intset->Delete(*victim));
        model.erase(victim);
        expect_matches_model();
      }
    }
  }
}

TEST(IntsetTest, RejectsInvalidRedisBlobs) {
  const std::string valid = Blob(4, {1, 2, 3});
  ASSERT_NE(Intset::CreateFromRedisBlob(valid), nullptr);
  // Order is that of the values, not of their bytes.
  ASSERT_NE(Intset::CreateFromRedisBlob(Blob(2, {-1, 1})), nullptr);

  const std::string three_values = valid.substr(8);
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"no bytes", ""},
      {"a header cut short", valid.substr(0, 7)},
      {"a width of 0", RawBlob(0, 0, "")},
      {"a width of 1", RawBlob(1, 2, std::string_view("\x01\x02", 2))},
      {"a width of 3", RawBlob(3, 2, std::string(6, '\x01'))},
      {"a width of 16", RawBlob(16, 1, std::string(16, '\0'))},
      // 2 in its low bytes only.
      {"a width of 65538", RawBlob(0x10002, 1, std::string_view("\x01\0", 2))},
      {"a length above the values", RawBlob(4, 4, three_values)},
      {"a length below the values", RawBlob(4, 2, three_values)},
      {"a length without values", RawBlob(4, 1, "")},
      // 2^29 values of 8 bytes are 2^32 bytes.
      {"a length that overflows 32 bits", RawBlob(8, 0x20000000, "")},
      {"a last value cut short", valid.substr(0, valid.size() - 1)},
      {"a byte after the last value", valid + '\x04'},
      {"values out of order", Blob(4, {1, 3, 2})},
      {"a value twice", Blob(4, {1, 2, 2})},
      {"descending values", Blob(8, {INT64_MAX, INT64_MIN})},
      {"a negative value after a positive one", Blob(2, {1, -1})},
  };
  for (const auto& [name, blob] : cases) {
    EXPECT_EQ(Intset::CreateFromRedisBlob(blob), nullptr) << name;
  }
}
}  // namespace redis_simple::containers
