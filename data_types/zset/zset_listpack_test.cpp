#include "data_types/zset/zset_listpack.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "containers/listpack.h"
#include "data_types/zset/zset_storage_test_util.h"

namespace redis_simple::data_types {
TEST(ZSetListpackTest, Add) { zset_storage_test::TestAdd<ZSetListpack>(); }

TEST(ZSetListpackTest, AddRulesForNewMember) {
  zset_storage_test::TestAddRulesForNewMember<ZSetListpack>();
}

TEST(ZSetListpackTest, AddRulesForExistingMember) {
  zset_storage_test::TestAddRulesForExistingMember<ZSetListpack>();
}

TEST(ZSetListpackTest, AddIncrementToNotANumber) {
  zset_storage_test::TestAddIncrementToNotANumber<ZSetListpack>();
}

TEST(ZSetListpackTest, Rank) { zset_storage_test::TestRank<ZSetListpack>(); }

TEST(ZSetListpackTest, Update) {
  zset_storage_test::TestUpdate<ZSetListpack>();
}

TEST(ZSetListpackTest, RangeByRank) {
  zset_storage_test::TestRangeByRank<ZSetListpack>();
}

namespace {
using zset_storage_test::MemberScorePair;

struct Limit {
  size_t offset;
  std::optional<size_t> count;
};

// What a rank range returns from a sorted set that holds `members`, which
// are in score order: the ranks from `min` to `max` of the order the range
// reads in, without an end that is excluded, and of those the part the limit
// keeps.
std::vector<MemberScorePair> ExpectedRankRange(
    std::vector<MemberScorePair> members, size_t min, size_t max, bool minex,
    bool maxex, const Limit& limit, bool reverse) {
  if (reverse) {
    std::ranges::reverse(members);
  }
  const size_t last = std::min(max, members.size() - 1);
  std::vector<MemberScorePair> in_range;
  in_range.reserve(members.size());
  for (size_t rank = min; rank <= last; ++rank) {
    if ((minex && rank == min) || (maxex && rank == last)) {
      continue;
    }
    in_range.push_back(members[rank]);
  }
  if (limit.offset >= in_range.size()) {
    return {};
  }
  const size_t kept = std::min(limit.count.value_or(in_range.size()),
                               in_range.size() - limit.offset);
  const auto first =
      in_range.begin() + static_cast<std::ptrdiff_t>(limit.offset);
  return {first, first + static_cast<std::ptrdiff_t>(kept)};
}

// Reads the ranks from `min` to `max` of `zset`, which holds `members`, with
// each choice of exclusive ends and direction and with limits that keep
// none, some and all of the range, and compares each result with
// ExpectedRankRange.
void ExpectRankRangesMatchModel(const ZSetListpack& zset,
                                const std::vector<MemberScorePair>& members,
                                size_t min, size_t max) {
  constexpr size_t kGreatest = std::numeric_limits<size_t>::max();
  const std::vector<Limit> limits = {
      {.offset = 0, .count = std::nullopt},
      {.offset = 0, .count = 0},
      {.offset = 0, .count = 1},
      {.offset = 1, .count = std::nullopt},
      {.offset = 2, .count = 3},
      {.offset = 3, .count = kGreatest},
      {.offset = members.size() - 1, .count = 1},
      {.offset = members.size(), .count = 1},
      {.offset = kGreatest, .count = std::nullopt},
      {.offset = kGreatest, .count = kGreatest},
  };
  for (const bool minex : {false, true}) {
    for (const bool maxex : {false, true}) {
      for (const bool reverse : {false, true}) {
        for (const Limit& limit : limits) {
          const RangeByRankSpec spec(
              static_cast<int64_t>(min), static_cast<int64_t>(max), minex,
              maxex, zset_storage_test::MakeLimit(limit.offset, limit.count),
              reverse);
          EXPECT_EQ(
              zset_storage_test::ToMemberScorePairs(zset.RangeByRank(&spec)),
              ExpectedRankRange(members, min, max, minex, maxex, limit,
                                reverse))
              << "ranks " << min << " to " << max << ", minex " << minex
              << ", maxex " << maxex << ", reverse " << reverse << ", offset "
              << limit.offset << ", count "
              << (limit.count.has_value() ? std::to_string(*limit.count)
                                          : std::string("none"));
        }
      }
    }
  }
}
}  // namespace

// A rank range seeks to its first member, so every range of a small sorted
// set is read, with ends on and past the last rank.
TEST(ZSetListpackTest, RangeByRankMatchesAModelForEveryRange) {
  // In score order. "e" and "f" have the same score, and whole-number scores
  // are stored as integers, the others as text.
  const std::vector<MemberScorePair> members = {
      {"a", -2.5}, {"b", -1.0}, {"c", 0.0},  {"d", 0.5},    {"e", 3.0},
      {"f", 3.0},  {"g", 3.25}, {"h", 1e15}, {"i", 1.5e300}};
  ZSetListpack zset;
  for (const size_t index : {4U, 0U, 8U, 2U, 6U, 1U, 7U, 3U, 5U}) {
    ASSERT_TRUE(zset.InsertOrUpdate(members[index].first, members[index].second,
                                    nullptr));
  }
  ASSERT_EQ(zset.Size(), members.size());

  for (size_t min = 0; min <= members.size() + 1; ++min) {
    for (size_t max = 0; max <= members.size() + 1; ++max) {
      ExpectRankRangesMatchModel(zset, members, min, max);
    }
  }
}

// A seek walks from the end of the listpack that is closer, so each rank of
// a sorted set of the largest size kept as a listpack is read on its own.
TEST(ZSetListpackTest, RangeByRankReadsEachRankOfAFullListpack) {
  constexpr size_t kSize = 128;
  // Members that a listpack stores as integers, at scores of which every
  // other one has a fraction.
  std::vector<MemberScorePair> members;
  members.reserve(kSize);
  for (size_t i = 0; i < kSize; ++i) {
    members.emplace_back(std::to_string(i), static_cast<double>(i) / 2);
  }
  ZSetListpack zset;
  for (size_t i = 0; i < kSize; ++i) {
    // 37 has no factor in common with 128, so this adds each member once.
    const MemberScorePair& member = members[(i * 37) % kSize];
    ASSERT_TRUE(zset.InsertOrUpdate(member.first, member.second, nullptr));
  }
  ASSERT_EQ(zset.Size(), kSize);

  for (size_t rank = 0; rank < kSize; ++rank) {
    const auto signed_rank = static_cast<int64_t>(rank);
    const RangeByRankSpec forward(signed_rank, signed_rank, false, false);
    EXPECT_EQ(zset_storage_test::ToMemberScorePairs(zset.RangeByRank(&forward)),
              std::vector<MemberScorePair>{members[rank]})
        << rank;
    const RangeByRankSpec reversed(signed_rank, signed_rank, false, false,
                                   std::nullopt, true);
    EXPECT_EQ(
        zset_storage_test::ToMemberScorePairs(zset.RangeByRank(&reversed)),
        std::vector<MemberScorePair>{members[kSize - 1 - rank]})
        << rank;
  }
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

TEST(ZSetListpackTest, StoresPairsInScoreOrderInRedisByteFormat) {
  ZSetListpack zset;
  // Inserted at the tail, the head and the middle, then moved by an update.
  ASSERT_TRUE(zset.InsertOrUpdate("b", 2.0, nullptr));
  ASSERT_TRUE(zset.InsertOrUpdate("d", 4.5, nullptr));
  ASSERT_TRUE(zset.InsertOrUpdate("a", 1.0, nullptr));
  ASSERT_TRUE(zset.InsertOrUpdate("c", 3.0, nullptr));
  bool changed = false;
  ASSERT_FALSE(zset.InsertOrUpdate("a", 3.0, &changed));
  EXPECT_TRUE(changed);

  // Whole-number scores are stored as integers, the others as text.
  containers::Listpack expected;
  ASSERT_TRUE(expected.Append("b"));
  ASSERT_TRUE(expected.Append(int64_t{2}));
  ASSERT_TRUE(expected.Append("a"));
  ASSERT_TRUE(expected.Append(int64_t{3}));
  ASSERT_TRUE(expected.Append("c"));
  ASSERT_TRUE(expected.Append(int64_t{3}));
  ASSERT_TRUE(expected.Append("d"));
  ASSERT_TRUE(expected.Append("4.5"));
  EXPECT_EQ(zset.ListpackBytes(), expected.Bytes());
}

// Members with the same score are kept in member order, which compares them
// as bytes, so "10" sorts before "9". Those two are stored as integers.
TEST(ZSetListpackTest, KeepsMembersWithOneScoreInMemberOrder) {
  ZSetListpack zset;
  for (const std::string_view member : {"b", "9", "d", "a", "10", "c"}) {
    ASSERT_TRUE(zset.InsertOrUpdate(member, 1.0, nullptr));
  }
  // A score on each side of theirs.
  ASSERT_TRUE(zset.InsertOrUpdate("z", 0.5, nullptr));
  ASSERT_TRUE(zset.InsertOrUpdate("0", 1.5, nullptr));

  const RangeByRankSpec all(0, -1, false, false);
  EXPECT_EQ(zset_storage_test::ToMemberScorePairs(zset.RangeByRank(&all)),
            (std::vector<MemberScorePair>{{"z", 0.5},
                                          {"10", 1.0},
                                          {"9", 1.0},
                                          {"a", 1.0},
                                          {"b", 1.0},
                                          {"c", 1.0},
                                          {"d", 1.0},
                                          {"0", 1.5}}));
}

TEST(ZSetListpackTest, RangeByScore) {
  zset_storage_test::TestRangeByScore<ZSetListpack>();
}

TEST(ZSetListpackTest, Count) { zset_storage_test::TestCount<ZSetListpack>(); }

TEST(ZSetListpackTest, Delete) {
  zset_storage_test::TestDelete<ZSetListpack>();
}
}  // namespace redis_simple::data_types
