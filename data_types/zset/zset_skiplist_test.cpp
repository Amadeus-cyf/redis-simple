#include "data_types/zset/zset_skiplist.h"

#include <gtest/gtest.h>

#include <vector>

#include "data_types/zset/zset_storage_test_util.h"

namespace redis_simple::zset {
using zset_storage_test::KeyScorePair;

TEST(ZSetSkiplistTest, Add) { zset_storage_test::TestAdd<ZSetSkiplist>(); }

TEST(ZSetSkiplistTest, Rank) { zset_storage_test::TestRank<ZSetSkiplist>(); }

TEST(ZSetSkiplistTest, Update) {
  zset_storage_test::TestUpdate<ZSetSkiplist>();
}

TEST(ZSetSkiplistTest, RangeByRank) {
  zset_storage_test::TestRangeByRank<ZSetSkiplist>();
}

TEST(ZSetSkiplistTest, RangeByScore) {
  zset_storage_test::TestRangeByScore<ZSetSkiplist>();
}

TEST(ZSetSkiplistTest, Count) { zset_storage_test::TestCount<ZSetSkiplist>(); }

TEST(ZSetSkiplistTest, Delete) {
  zset_storage_test::TestDelete<ZSetSkiplist>();
}

TEST(ZSetSkiplistStandaloneTest, EmptyRangeByScoreAndCountAreSafe) {
  ZSetSkiplist zset_skiplist;
  const RangeByScoreSpec spec(0.0, 10.0, false, false);

  ASSERT_TRUE(zset_skiplist.RangeByScore(&spec).empty());
  ASSERT_EQ(zset_skiplist.Count(&spec), 0);
}

TEST(ZSetSkiplistStandaloneTest, DeleteRecomputesRangeBoundaries) {
  ZSetSkiplist zset_skiplist;
  ASSERT_TRUE(zset_skiplist.InsertOrUpdate("a", 1.0));
  ASSERT_TRUE(zset_skiplist.InsertOrUpdate("b", 2.0));
  ASSERT_TRUE(zset_skiplist.InsertOrUpdate("c", 3.0));

  ASSERT_TRUE(zset_skiplist.Delete("a"));
  ASSERT_TRUE(zset_skiplist.Delete("c"));

  const RangeByScoreSpec spec(0.0, 10.0, false, false);
  const auto pairs =
      zset_storage_test::ToKeyScorePairs(zset_skiplist.RangeByScore(&spec));
  ASSERT_EQ(pairs, std::vector<KeyScorePair>({{"b", 2.0}}));
  ASSERT_EQ(zset_skiplist.Count(&spec), 1);
}
}  // namespace redis_simple::zset
