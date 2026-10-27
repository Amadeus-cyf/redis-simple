#include "data_types/zset/zset_listpack.h"

#include <gtest/gtest.h>

#include "data_types/zset/zset_storage_test_util.h"

namespace redis_simple::zset {
TEST(ZSetListPackTest, Add) { zset_storage_test::TestAdd<ZSetListPack>(); }

TEST(ZSetListPackTest, Rank) { zset_storage_test::TestRank<ZSetListPack>(); }

TEST(ZSetListPackTest, Update) {
  zset_storage_test::TestUpdate<ZSetListPack>();
}

TEST(ZSetListPackTest, RangeByRank) {
  zset_storage_test::TestRangeByRank<ZSetListPack>();
}

TEST(ZSetListPackTest, PreservesNumericKeyWhileReadingScore) {
  ZSetListPack zset;
  ASSERT_TRUE(zset.InsertOrUpdate("0", 42.0));
  const RangeByRankSpec spec(0, 0, false, false);

  const auto entries = zset.RangeByRank(&spec);

  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries.front()->key, "0");
  EXPECT_EQ(entries.front()->score, 42.0);
}

TEST(ZSetListPackTest, RangeByScore) {
  zset_storage_test::TestRangeByScore<ZSetListPack>();
}

TEST(ZSetListPackTest, Count) { zset_storage_test::TestCount<ZSetListPack>(); }

TEST(ZSetListPackTest, Delete) {
  zset_storage_test::TestDelete<ZSetListPack>();
}
}  // namespace redis_simple::zset
