#include "data_types/zset/zset_listpack.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "data_types/zset/zset_storage_test_util.h"

namespace redis_simple::data_types {
TEST(ZSetListpackTest, Add) { zset_storage_test::TestAdd<ZSetListpack>(); }

TEST(ZSetListpackTest, Rank) { zset_storage_test::TestRank<ZSetListpack>(); }

TEST(ZSetListpackTest, Update) {
  zset_storage_test::TestUpdate<ZSetListpack>();
}

TEST(ZSetListpackTest, RangeByRank) {
  zset_storage_test::TestRangeByRank<ZSetListpack>();
}

TEST(ZSetListpackTest, PreservesNumericMemberWhileReadingScore) {
  ZSetListpack zset;
  ASSERT_TRUE(zset.InsertOrUpdate("0", 42.0, nullptr));
  const RangeByRankSpec spec(0, 0, false, false);

  const ZSetEntryList entries = zset.RangeByRank(&spec);

  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries.front().member, "0");
  EXPECT_EQ(entries.front().score, 42.0);
}

TEST(ZSetListpackTest, StoresNegativeZeroAsZero) {
  // Redis stores whole-number listpack scores as integers, so -0 reads back
  // as 0; the skiplist encoding keeps the sign.
  ZSetListpack zset;
  ASSERT_TRUE(zset.InsertOrUpdate("member", -0.0, nullptr));
  const double score = zset.Score("member").value_or(1.0);
  EXPECT_EQ(score, 0.0);
  EXPECT_FALSE(std::signbit(score));
}

TEST(ZSetListpackTest, ReadsIntegralAndFractionalScoresExactly) {
  // Integral scores take the listpack's integer encodings; the others are
  // stored as text. Both must read back as the exact double inserted.
  const std::vector<std::pair<std::string, double>> entries = {
      {"min", -9007199254740992.0},
      {"neg", -42.0},
      {"zero", 0.0},
      {"small", 7.0},
      {"half", 7.5},
      {"big", 1e15},
      {"max", 9007199254740992.0},
      {"huge", 1.5e300},
  };
  ZSetListpack zset;
  for (const auto& [member, score] : entries) {
    ASSERT_TRUE(zset.InsertOrUpdate(member, score, nullptr));
  }
  for (const auto& [member, score] : entries) {
    EXPECT_EQ(zset.Score(member), score) << member;
  }
  const RangeByRankSpec spec(0, static_cast<int64_t>(entries.size()) - 1, false,
                             false);
  const ZSetEntryList range = zset.RangeByRank(&spec);
  ASSERT_EQ(range.size(), entries.size());
  for (size_t index = 0; index < entries.size(); ++index) {
    EXPECT_EQ(range[index].member, entries[index].first);
    EXPECT_EQ(range[index].score, entries[index].second);
  }
}

TEST(ZSetListpackTest, RangeByScore) {
  zset_storage_test::TestRangeByScore<ZSetListpack>();
}

TEST(ZSetListpackTest, Count) { zset_storage_test::TestCount<ZSetListpack>(); }

TEST(ZSetListpackTest, Delete) {
  zset_storage_test::TestDelete<ZSetListpack>();
}
}  // namespace redis_simple::data_types
