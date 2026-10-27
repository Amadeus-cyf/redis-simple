#include "data_types/zset/zset.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace redis_simple::zset {
TEST(ZSetTest, InsertAndConvert) {
  auto zset = ZSet::Create();
  for (int i = 0; i < 256; ++i) {
    std::string prefix("key_");
    prefix.append(std::to_string(i));
    ASSERT_TRUE(zset->InsertOrUpdate(prefix, i));
  }
  ASSERT_EQ(zset->Size(), 256);
  ASSERT_EQ(zset->Rank("key_0"), 0);
  ASSERT_EQ(zset->Rank("key_255"), 255);
  ASSERT_EQ(zset->Rank("key_256").value_or(-1), -1);

  RangeByRankSpec spec;
  spec.min = 0;
  spec.max = 255;
  spec.minex = false;
  spec.maxex = false;
  spec.limit.emplace(0, std::nullopt);
  spec.reverse = false;
  const auto entries = zset->RangeByRank(&spec);
  ASSERT_EQ(entries.size(), 256);
  ASSERT_EQ(entries.front()->key, "key_0");
  ASSERT_EQ(entries.back()->key, "key_255");
}

TEST(ZSetEncodingTest, ConvertsToSkiplistWhenMemberIsLong) {
  auto zset = ZSet::Create();
  ASSERT_TRUE(zset->InsertOrUpdate("short", 1.0));
  ASSERT_EQ(zset->Encoding(), ZSet::Encoding::kListPack);

  ASSERT_TRUE(zset->InsertOrUpdate(std::string(65, 'x'), 2.0));

  ASSERT_EQ(zset->Encoding(), ZSet::Encoding::kSkiplist);
  ASSERT_EQ(zset->Size(), 2);
  ASSERT_EQ(zset->Score("short"), 1.0);
}

TEST(ZSetEncodingTest, ConvertsToSkiplistWhenEntryCountExceedsLimit) {
  auto zset = ZSet::Create();
  for (int i = 0; i < 128; ++i) {
    ASSERT_TRUE(zset->InsertOrUpdate("key_" + std::to_string(i), i));
  }
  ASSERT_EQ(zset->Encoding(), ZSet::Encoding::kListPack);

  ASSERT_TRUE(zset->InsertOrUpdate("key_128", 128.0));

  ASSERT_EQ(zset->Encoding(), ZSet::Encoding::kSkiplist);
  ASSERT_EQ(zset->Size(), 129);
  ASSERT_EQ(zset->Rank("key_0"), 0);
  ASSERT_EQ(zset->Rank("key_128"), 128);
}

TEST(ZSetTest, VisitsEntriesAcrossEncodings) {
  auto zset = ZSet::Create();
  ASSERT_TRUE(zset->InsertOrUpdate("first", 1.0));
  ASSERT_TRUE(zset->InsertOrUpdate("second", 2.0));

  std::vector<std::pair<std::string, double>> entries;
  ASSERT_TRUE(
      zset->ForEachEntry([&entries](std::string_view key, double score) {
        entries.emplace_back(key, score);
        return true;
      }));
  EXPECT_EQ(entries, (std::vector<std::pair<std::string, double>>{
                         {"first", 1.0}, {"second", 2.0}}));

  ASSERT_TRUE(zset->InsertOrUpdate(std::string(65, 'x'), 3.0));
  ASSERT_EQ(zset->Encoding(), ZSet::Encoding::kSkiplist);
  size_t visited = 0;
  ASSERT_TRUE(zset->ForEachEntry([&visited](std::string_view, double) {
    ++visited;
    return true;
  }));
  EXPECT_EQ(visited, zset->Size());
}
}  // namespace redis_simple::zset
