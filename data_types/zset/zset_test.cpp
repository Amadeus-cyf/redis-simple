#include "data_types/zset/zset.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
  const ZSetEntryList entries = zset->RangeByRank(&spec);
  ASSERT_EQ(entries.size(), 256);
  ASSERT_EQ(entries.front().member, "key_0");
  ASSERT_EQ(entries.back().member, "key_255");
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
      zset->ForEachEntry([&entries](std::string_view member, double score) {
        entries.emplace_back(member, score);
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

TEST(ZSetEncodingTest, ScanSkiplistVisitsEveryMemberOnce) {
  auto zset = ZSet::Create();
  for (int i = 0; i < 300; ++i) {
    zset->InsertOrUpdate("member" + std::to_string(i), i / 2.0);
  }
  ASSERT_EQ(zset->Encoding(), ZSet::Encoding::kSkiplist);
  std::map<std::string, double> seen;
  size_t cursor = 0;
  while (true) {
    cursor =
        zset->Scan(cursor, 5, [&seen](std::string_view member, double score) {
          EXPECT_TRUE(seen.emplace(member, score).second) << member;
        });
    if (cursor == 0) {
      break;
    }
  }
  ASSERT_EQ(seen.size(), 300);
  EXPECT_EQ(seen["member7"], 3.5);
}
}  // namespace redis_simple::zset
