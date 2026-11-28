#include "data_types/zset/zset_skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data_types/zset/zset_range_spec.h"
#include "data_types/zset/zset_storage_test_util.h"

namespace redis_simple::data_types {
using zset_storage_test::MemberScorePair;
using zset_storage_test::ToMemberScorePairs;

namespace {
constexpr size_t kMemberCount = 40;

// "m00" to "m39" in order. Member i has score i, or every member has score 0
// with `one_score`, which is what a lexicographic range needs.
std::vector<MemberScorePair> Members(bool one_score) {
  std::vector<MemberScorePair> members;
  for (size_t i = 0; i < kMemberCount; ++i) {
    members.emplace_back((i < 10 ? "m0" : "m") + std::to_string(i),
                         one_score ? 0.0 : static_cast<double>(i));
  }
  return members;
}

// Adds the members out of order.
void AddAll(const std::vector<MemberScorePair>& members,
            ZSetSkiplist* const zset) {
  for (size_t i = 0; i < members.size(); ++i) {
    const auto& [member, score] = members[(i * 7) % members.size()];
    ASSERT_TRUE(zset->InsertOrUpdate(member, score, nullptr));
  }
}

// members[first..last] in the order a range visits them, after `limit`.
std::vector<MemberScorePair> Expected(
    const std::vector<MemberScorePair>& members, size_t first, size_t last,
    bool reverse, const LimitSpec& limit) {
  std::vector<MemberScorePair> range(
      members.begin() + static_cast<std::ptrdiff_t>(first),
      members.begin() + static_cast<std::ptrdiff_t>(last + 1));
  if (reverse) {
    std::ranges::reverse(range);
  }
  if (limit.offset >= range.size()) {
    return {};
  }
  range.erase(range.begin(),
              range.begin() + static_cast<std::ptrdiff_t>(limit.offset));
  range.resize(std::min(range.size(), limit.count.value_or(range.size())));
  return range;
}

// Offsets on both sides of the one from which the skiplist reaches the first
// member by rank, and past the 30 members of the ranges below.
std::vector<LimitSpec> Limits() {
  std::vector<LimitSpec> limits;
  for (const size_t offset :
       std::vector<size_t>{0, 3, 9, 10, 11, 25, 29, 30, 31, 100,
                           std::numeric_limits<size_t>::max()}) {
    for (const std::optional<size_t> count :
         std::vector<std::optional<size_t>>{std::nullopt, 0U, 4U, 100U}) {
      limits.emplace_back(offset, count);
    }
  }
  return limits;
}

LexBound Lex(std::string_view value) {
  return {.kind = LexBound::Kind::kValue, .value = value};
}

std::string Describe(const LimitSpec& limit, bool reverse) {
  return "offset " + std::to_string(limit.offset) + ", count " +
         (limit.count.has_value() ? std::to_string(*limit.count) : "none") +
         (reverse ? ", reversed" : "");
}
}  // namespace

TEST(ZSetSkiplistTest, Add) { zset_storage_test::TestAdd<ZSetSkiplist>(); }

TEST(ZSetSkiplistTest, AddRulesForNewMember) {
  zset_storage_test::TestAddRulesForNewMember<ZSetSkiplist>();
}

TEST(ZSetSkiplistTest, AddRulesForExistingMember) {
  zset_storage_test::TestAddRulesForExistingMember<ZSetSkiplist>();
}

TEST(ZSetSkiplistTest, AddIncrementToNotANumber) {
  zset_storage_test::TestAddIncrementToNotANumber<ZSetSkiplist>();
}

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
  ASSERT_TRUE(zset_skiplist.InsertOrUpdate("a", 1.0, nullptr));
  ASSERT_TRUE(zset_skiplist.InsertOrUpdate("b", 2.0, nullptr));
  ASSERT_TRUE(zset_skiplist.InsertOrUpdate("c", 3.0, nullptr));

  ASSERT_TRUE(zset_skiplist.Delete("a"));
  ASSERT_TRUE(zset_skiplist.Delete("c"));

  const RangeByScoreSpec spec(0.0, 10.0, false, false);
  const std::vector<MemberScorePair> pairs =
      zset_storage_test::ToMemberScorePairs(zset_skiplist.RangeByScore(&spec));
  ASSERT_EQ(pairs, std::vector<MemberScorePair>({{"b", 2.0}}));
  ASSERT_EQ(zset_skiplist.Count(&spec), 1);
}

TEST(ZSetSkiplistStandaloneTest, ScoreRangeLimitsSkipTheOffset) {
  const std::vector<MemberScorePair> members = Members(/*one_score=*/false);
  ZSetSkiplist zset;
  AddAll(members, &zset);

  for (const bool reverse : {false, true}) {
    for (const LimitSpec& limit : Limits()) {
      SCOPED_TRACE(Describe(limit, reverse));
      const RangeByScoreSpec spec(5.0, 34.0, false, false, limit, reverse);
      EXPECT_EQ(ToMemberScorePairs(zset.RangeByScore(&spec)),
                Expected(members, 5, 34, reverse, limit));
      // Exclusive bounds leave out the members at both ends.
      const RangeByScoreSpec inner(4.0, 35.0, true, true, limit, reverse);
      EXPECT_EQ(ToMemberScorePairs(zset.RangeByScore(&inner)),
                Expected(members, 5, 34, reverse, limit));
    }
  }
  const RangeByScoreSpec spec(5.0, 34.0, false, false);
  EXPECT_EQ(zset.Count(&spec), 30);
}

TEST(ZSetSkiplistStandaloneTest, LexRangeLimitsSkipTheOffset) {
  const std::vector<MemberScorePair> members = Members(/*one_score=*/true);
  ZSetSkiplist zset;
  AddAll(members, &zset);

  for (const bool reverse : {false, true}) {
    for (const LimitSpec& limit : Limits()) {
      SCOPED_TRACE(Describe(limit, reverse));
      const RangeByLexSpec spec{.min = Lex("m05"),
                                .max = Lex("m34"),
                                .minex = false,
                                .maxex = false,
                                .limit = limit,
                                .reverse = reverse};
      EXPECT_EQ(ToMemberScorePairs(zset.RangeByLex(&spec)),
                Expected(members, 5, 34, reverse, limit));
      const RangeByLexSpec inner{.min = Lex("m04"),
                                 .max = Lex("m35"),
                                 .minex = true,
                                 .maxex = true,
                                 .limit = limit,
                                 .reverse = reverse};
      EXPECT_EQ(ToMemberScorePairs(zset.RangeByLex(&inner)),
                Expected(members, 5, 34, reverse, limit));
      // "-" and "+" reach the first and the last member.
      const RangeByLexSpec all{
          .min = {.kind = LexBound::Kind::kMinusInfinity, .value = {}},
          .max = {.kind = LexBound::Kind::kPlusInfinity, .value = {}},
          .minex = false,
          .maxex = false,
          .limit = limit,
          .reverse = reverse};
      EXPECT_EQ(ToMemberScorePairs(zset.RangeByLex(&all)),
                Expected(members, 0, kMemberCount - 1, reverse, limit));
    }
  }
  const RangeByLexSpec spec{.min = Lex("m05"),
                            .max = Lex("m34"),
                            .minex = false,
                            .maxex = false,
                            .limit = std::nullopt,
                            .reverse = false};
  EXPECT_EQ(zset.LexCount(&spec), 30);
}

TEST(ZSetSkiplistStandaloneTest, LexRangeOutsideTheMembersIsEmpty) {
  const std::vector<MemberScorePair> members = Members(/*one_score=*/true);
  ZSetSkiplist zset;
  AddAll(members, &zset);

  struct Bounds {
    LexBound min;
    LexBound max;
    bool minex;
    bool maxex;
    size_t count;
  };
  const LexBound minus{.kind = LexBound::Kind::kMinusInfinity, .value = {}};
  const LexBound plus{.kind = LexBound::Kind::kPlusInfinity, .value = {}};
  const std::vector<Bounds> cases{
      // Before the first member and after the last one.
      {Lex("a"), Lex("b"), false, false, 0},
      {Lex("n"), Lex("z"), false, false, 0},
      {minus, Lex("m00"), false, true, 0},
      {Lex("m39"), plus, true, false, 0},
      // Between two members, and with the bounds out of order.
      {Lex("m100"), Lex("m109"), false, false, 0},
      {Lex("m20"), Lex("m10"), false, false, 0},
      {plus, minus, false, false, 0},
      // The first and the last member alone.
      {minus, Lex("m00"), false, false, 1},
      {Lex("m39"), plus, false, false, 1},
  };
  for (const Bounds& bounds : cases) {
    for (const bool reverse : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << bounds.min.value << " to " << bounds.max.value
                   << (reverse ? ", reversed" : ""));
      const RangeByLexSpec spec{.min = bounds.min,
                                .max = bounds.max,
                                .minex = bounds.minex,
                                .maxex = bounds.maxex,
                                .limit = std::nullopt,
                                .reverse = reverse};
      EXPECT_EQ(zset.RangeByLex(&spec).size(), bounds.count);
      EXPECT_EQ(zset.LexCount(&spec), bounds.count);
    }
  }
}

TEST(ZSetSkiplistStandaloneTest, DeleteRemovesTheMemberFromLookupsAndRanges) {
  // Enough members that the member dict resizes while they are deleted, with
  // scores that tie.
  constexpr size_t kCount = 3000;
  ZSetSkiplist zset;
  const auto name = [](size_t i) { return "member:" + std::to_string(i); };
  const auto score = [](size_t i) { return static_cast<double>(i % 97); };
  for (size_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(zset.InsertOrUpdate(name(i), score(i), nullptr));
  }

  // 613 and 3000 share no factor, so this visits every member once.
  std::vector<MemberScorePair> kept;
  for (size_t i = 0; i < kCount; ++i) {
    const size_t member = (i * 613) % kCount;
    if (member % 8 == 0) {
      kept.emplace_back(name(member), score(member));
      continue;
    }
    ASSERT_TRUE(zset.Delete(name(member)));
    ASSERT_FALSE(zset.Delete(name(member)));
    ASSERT_FALSE(zset.Score(name(member)).has_value());
    ASSERT_FALSE(zset.Rank(name(member)).has_value());
  }
  EXPECT_FALSE(zset.Delete("member:absent"));
  ASSERT_EQ(zset.Size(), kept.size());

  std::ranges::sort(
      kept, [](const MemberScorePair& a, const MemberScorePair& b) {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
      });
  const RangeByRankSpec all(0, -1, false, false);
  EXPECT_EQ(ToMemberScorePairs(zset.RangeByRank(&all)), kept);
  for (size_t rank = 0; rank < kept.size(); ++rank) {
    ASSERT_EQ(zset.Rank(kept[rank].first), rank);
    ASSERT_EQ(zset.Score(kept[rank].first), kept[rank].second);
  }

  for (const MemberScorePair& member : kept) {
    ASSERT_TRUE(zset.Delete(member.first));
  }
  EXPECT_EQ(zset.Size(), 0);
  EXPECT_TRUE(zset.RangeByRank(&all).empty());
}
}  // namespace redis_simple::data_types
