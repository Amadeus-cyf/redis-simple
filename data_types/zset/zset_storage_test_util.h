#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_TEST_UTIL_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_TEST_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "data_types/zset/zset_storage.h"
#include "gtest/gtest.h"

namespace redis_simple::zset::zset_storage_test {
using KeyScorePair = std::pair<std::string, double>;

inline std::optional<LimitSpec> MakeLimit(size_t offset,
                                          std::optional<size_t> count) {
  return LimitSpec(offset, count);
}

inline std::vector<KeyScorePair> ToKeyScorePairs(const ZSetEntryList& entries) {
  std::vector<KeyScorePair> pairs;
  pairs.reserve(entries.size());
  for (const auto* entry : entries) {
    pairs.emplace_back(entry->key, entry->score);
  }
  return pairs;
}

template <typename Storage>
std::unique_ptr<Storage> MakeBaseStorage() {
  auto zset = std::make_unique<Storage>();
  zset->InsertOrUpdate("key1", 3.0);
  zset->InsertOrUpdate("key2", 2.0);
  zset->InsertOrUpdate("key3", 1.0);
  zset->InsertOrUpdate("key4", 1.0);
  return zset;
}

template <typename Storage>
std::unique_ptr<Storage> MakeUpdatedStorage() {
  auto zset = MakeBaseStorage<Storage>();
  zset->InsertOrUpdate("key1", 10.0);
  zset->InsertOrUpdate("key1", 1.0);
  zset->InsertOrUpdate("key3", 4.0);
  zset->InsertOrUpdate("key1", 5.0);
  return zset;
}

template <typename Storage>
void ExpectRank(const Storage& zset, const std::string& key,
                size_t expected_rank) {
  const auto rank = zset.Rank(key);
  ASSERT_TRUE(rank.has_value()) << key;
  EXPECT_EQ(*rank, expected_rank) << key;
}

template <typename Storage>
void ExpectRangeByRank(const Storage& zset, int64_t min, int64_t max,
                       bool minex, bool maxex, size_t offset,
                       std::optional<size_t> count, bool reverse,
                       const std::vector<KeyScorePair>& expected) {
  const RangeByRankSpec spec(min, max, minex, maxex, MakeLimit(offset, count),
                             reverse);
  EXPECT_EQ(ToKeyScorePairs(zset.RangeByRank(&spec)), expected);
}

template <typename Storage>
void ExpectRangeByScore(const Storage& zset, double min, double max, bool minex,
                        bool maxex, size_t offset, std::optional<size_t> count,
                        bool reverse,
                        const std::vector<KeyScorePair>& expected) {
  const RangeByScoreSpec spec(min, max, minex, maxex, MakeLimit(offset, count),
                              reverse);
  EXPECT_EQ(ToKeyScorePairs(zset.RangeByScore(&spec)), expected);
}

template <typename Storage>
void ExpectCount(const Storage& zset, double min, double max, bool minex,
                 bool maxex, size_t expected_count) {
  const RangeByScoreSpec spec(min, max, minex, maxex);
  EXPECT_EQ(zset.Count(&spec), expected_count);
}

template <typename Storage>
void TestAdd() {
  auto zset = std::make_unique<Storage>();
  ASSERT_FALSE(zset->InsertOrUpdate("not-a-number",
                                    std::numeric_limits<double>::quiet_NaN()));
  ASSERT_EQ(zset->Size(), 0);
  ASSERT_TRUE(zset->InsertOrUpdate("key1", 3.0));
  ASSERT_EQ(zset->Size(), 1);

  ASSERT_TRUE(zset->InsertOrUpdate("key2", 2.0));
  ASSERT_EQ(zset->Size(), 2);

  ASSERT_TRUE(zset->InsertOrUpdate("key3", 1.0));
  ASSERT_EQ(zset->Size(), 3);

  ASSERT_TRUE(zset->InsertOrUpdate("key4", 1.0));
  ASSERT_EQ(zset->Size(), 4);
}

template <typename Storage>
void TestRank() {
  const auto zset = MakeBaseStorage<Storage>();
  ExpectRank(*zset, "key1", 3);
  ExpectRank(*zset, "key2", 2);
  ExpectRank(*zset, "key3", 0);
  ExpectRank(*zset, "key4", 1);
  ASSERT_FALSE(zset->Rank("key_not_exist").has_value());
}

template <typename Storage>
void TestUpdate() {
  auto zset = MakeBaseStorage<Storage>();

  ASSERT_FALSE(zset->InsertOrUpdate("key1", 10.0));
  ASSERT_EQ(zset->Size(), 4);
  ExpectRank(*zset, "key1", 3);

  ASSERT_FALSE(zset->InsertOrUpdate("key1", 1.0));
  ASSERT_EQ(zset->Size(), 4);
  ASSERT_FALSE(zset->InsertOrUpdate("key3", 4.0));
  ASSERT_EQ(zset->Size(), 4);

  ExpectRank(*zset, "key1", 0);
  ExpectRank(*zset, "key2", 2);
  ExpectRank(*zset, "key3", 3);
  ExpectRank(*zset, "key4", 1);

  ASSERT_FALSE(zset->InsertOrUpdate("key1", 5.0));
  ASSERT_EQ(zset->Size(), 4);

  ExpectRank(*zset, "key1", 3);
  ExpectRank(*zset, "key4", 0);
}

template <typename Storage>
void TestRangeByRank() {
  const auto zset = MakeUpdatedStorage<Storage>();
  EXPECT_TRUE(zset->RangeByRank(nullptr).empty());
  ExpectRangeByRank(
      *zset, 0, 3, false, false, 0, std::nullopt, false,
      {{"key4", 1.0}, {"key2", 2.0}, {"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByRank(*zset, 1, 3, true, false, 0, std::nullopt, false,
                    {{"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByRank(*zset, 1, 3, false, true, 0, std::nullopt, false,
                    {{"key2", 2.0}, {"key3", 4.0}});
  ExpectRangeByRank(*zset, 1, 3, true, true, 0, std::nullopt, false,
                    {{"key3", 4.0}});
  ExpectRangeByRank(*zset, 0, 3, false, false, 2, 3, false,
                    {{"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByRank(*zset, 1, 3, false, false, 1, std::nullopt, false,
                    {{"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByRank(
      *zset, 0, 3, false, false, 0, std::nullopt, true,
      {{"key1", 5.0}, {"key3", 4.0}, {"key2", 2.0}, {"key4", 1.0}});
  ExpectRangeByRank(*zset, 0, 3, false, false, 1, 2, true,
                    {{"key3", 4.0}, {"key2", 2.0}});
  ExpectRangeByRank(*zset, 0, 3, false, false, 0, 0, false, {});
  ExpectRangeByRank(*zset, 2, 1, false, false, 0, std::nullopt, false, {});
  ExpectRangeByRank(*zset, 1, 1, false, true, 0, std::nullopt, false, {});
  ExpectRangeByRank(*zset, 10, 12, false, false, 0, std::nullopt, false, {});
  ExpectRangeByRank(*zset, 0, 3, false, false, 10, std::nullopt, false, {});
  ExpectRangeByRank(*zset, -3, -1, false, false, 0, std::nullopt, false,
                    {{"key2", 2.0}, {"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByRank(*zset, -3, -1, true, false, 0, std::nullopt, false,
                    {{"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByRank(*zset, -3, -1, false, true, 0, std::nullopt, false,
                    {{"key2", 2.0}, {"key3", 4.0}});
  ExpectRangeByRank(*zset, -3, -1, false, false, 0, std::nullopt, true,
                    {{"key3", 4.0}, {"key2", 2.0}, {"key4", 1.0}});
  ExpectRangeByRank(*zset, -1, -3, false, false, 0, std::nullopt, false, {});
  ExpectRangeByRank(*zset, -1, -1, true, false, 0, std::nullopt, false, {});
  ExpectRangeByRank(
      *zset, -10, 3, false, false, 0, std::nullopt, false,
      {{"key4", 1.0}, {"key2", 2.0}, {"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByRank(*zset, -1, -6, false, false, 0, std::nullopt, false, {});
  ExpectRangeByRank(
      *zset, 0, std::numeric_limits<int64_t>::max(), false, false, 0,
      std::nullopt, false,
      {{"key4", 1.0}, {"key2", 2.0}, {"key3", 4.0}, {"key1", 5.0}});
}

template <typename Storage>
void TestRangeByScore() {
  const auto zset = MakeUpdatedStorage<Storage>();
  const auto infinity = std::numeric_limits<double>::infinity();
  EXPECT_TRUE(zset->RangeByScore(nullptr).empty());

  ExpectRangeByScore(
      *zset, 1.0, 5.0, false, false, 0, std::nullopt, false,
      {{"key4", 1.0}, {"key2", 2.0}, {"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByScore(
      *zset, -infinity, infinity, false, false, 0, std::nullopt, false,
      {{"key4", 1.0}, {"key2", 2.0}, {"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByScore(*zset, 2.0, 5.0, true, false, 0, std::nullopt, false,
                     {{"key3", 4.0}, {"key1", 5.0}});
  ExpectRangeByScore(*zset, 2.0, 5.0, false, true, 0, std::nullopt, false,
                     {{"key2", 2.0}, {"key3", 4.0}});
  ExpectRangeByScore(*zset, 2.0, 5.0, true, true, 0, std::nullopt, false,
                     {{"key3", 4.0}});
  ExpectRangeByScore(*zset, 1.0, 6.0, false, false, 1, 2, false,
                     {{"key2", 2.0}, {"key3", 4.0}});
  ExpectRangeByScore(*zset, 2.0, 6.0, false, false, 0, std::nullopt, true,
                     {{"key1", 5.0}, {"key3", 4.0}, {"key2", 2.0}});
  ExpectRangeByScore(*zset, 1.0, 6.0, false, false, 1, 2, true,
                     {{"key3", 4.0}, {"key2", 2.0}});
  ExpectRangeByScore(*zset, 1.0, 6.0, false, false, 0, 0, false, {});
  ExpectRangeByScore(*zset, 6.0, 1.0, false, false, 0, std::nullopt, false, {});
  ExpectRangeByScore(*zset, 1.0, 1.0, true, false, 0, std::nullopt, false, {});
  ExpectRangeByScore(*zset, 1.0, 1.0, false, true, 0, std::nullopt, false, {});
  ExpectRangeByScore(*zset, 1.0, 6.0, false, false, 5, 0, false, {});
}

template <typename Storage>
void TestCount() {
  const auto zset = MakeUpdatedStorage<Storage>();
  EXPECT_EQ(zset->Count(nullptr), 0);
  ExpectCount(*zset, 1.0, 6.0, false, false, 4);
  ExpectCount(*zset, 2.0, 5.0, true, false, 2);
  ExpectCount(*zset, 2.0, 5.0, false, true, 2);
  ExpectCount(*zset, 1.0, 5.0, true, true, 2);
  ExpectCount(*zset, 6.0, 1.0, false, false, 0);
  ExpectCount(*zset, 1.0, 1.0, true, false, 0);
  ExpectCount(*zset, 1.0, 1.0, false, true, 0);
}

template <typename Storage>
void TestDelete() {
  auto zset = MakeUpdatedStorage<Storage>();
  ASSERT_TRUE(zset->Delete("key1"));
  ASSERT_EQ(zset->Size(), 3);
  ASSERT_FALSE(zset->Rank("key1").has_value());
  ASSERT_FALSE(zset->Delete("key not exist"));
}
}  // namespace redis_simple::zset::zset_storage_test

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_TEST_UTIL_H_
