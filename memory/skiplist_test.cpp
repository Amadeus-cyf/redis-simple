#include "memory/skiplist.h"

#include <gtest/gtest.h>

#include <climits>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace redis_simple::in_memory {
namespace {
using StringSkiplist = Skiplist<std::string>;
using Limit = StringSkiplist::Limit;
using RankRange = StringSkiplist::RankRange;
using KeyRange = StringSkiplist::KeyRange<std::string>;

struct RankRangeTestCase {
  RankRange range;
  std::vector<std::string> keys;
  std::vector<std::string> reversed_keys;
};

struct KeyRangeTestCase {
  KeyRange range;
  std::vector<std::string> keys;
  std::vector<std::string> reversed_keys;
  // Expected Count(), which ignores the limit.
  size_t count;
};

RankRange Ranks(size_t min, size_t max, bool min_exclusive, bool max_exclusive,
                std::optional<Limit> limit = std::nullopt) {
  return {.min = min,
          .max = max,
          .min_exclusive = min_exclusive,
          .max_exclusive = max_exclusive,
          .limit = limit,
          .reverse = false};
}

KeyRange Keys(std::string min, bool min_exclusive, std::string max,
              bool max_exclusive, std::optional<Limit> limit = std::nullopt) {
  return {.min = std::move(min),
          .max = std::move(max),
          .min_exclusive = min_exclusive,
          .max_exclusive = max_exclusive,
          .limit = limit,
          .reverse = false};
}

template <typename Range>
Range Reversed(Range range) {
  range.reverse = true;
  return range;
}

std::unique_ptr<StringSkiplist> MakeInsertedSkiplist() {
  auto skiplist = std::make_unique<StringSkiplist>(4);
  skiplist->Insert("key1");
  skiplist->Insert("key2");
  skiplist->Insert("key0");
  return skiplist;
}

std::unique_ptr<StringSkiplist> MakeRankedSkiplist() {
  auto skiplist = std::make_unique<StringSkiplist>(4);
  skiplist->Insert("key0");
  skiplist->Insert("key2");
  skiplist->Insert("key4");
  skiplist->Insert("key5");
  return skiplist;
}

std::vector<RankRangeTestCase> RankRangeTestCases() {
  const Limit unbounded{.offset = 0, .count = std::nullopt};
  const Limit first_two{.offset = 0, .count = 2};
  const Limit skip_two{.offset = 2, .count = 3};
  const Limit skip_all{.offset = 10, .count = std::nullopt};
  return {
      {Ranks(0, 3, false, false),
       {"key0", "key2", "key4", "key5"},
       {"key5", "key4", "key2", "key0"}},
      {Ranks(1, 3, true, false), {"key4", "key5"}, {"key2", "key0"}},
      {Ranks(1, 3, false, true), {"key2", "key4"}, {"key4", "key2"}},
      {Ranks(1, 3, false, false, unbounded),
       {"key2", "key4", "key5"},
       {"key4", "key2", "key0"}},
      {Ranks(1, 3, false, false, first_two),
       {"key2", "key4"},
       {"key4", "key2"}},
      {Ranks(0, 3, false, false, skip_two), {"key4", "key5"}, {"key2", "key0"}},
      {Ranks(0, 4, false, false, skip_all), {}, {}},
      {Ranks(2, 1, false, false), {}, {}},
      {Ranks(1, 1, true, false), {}, {}},
      {Ranks(1, 1, false, true), {}, {}},
      {Ranks(100000, 1000000, false, false), {}, {}},
  };
}

std::vector<KeyRangeTestCase> KeyRangeTestCases() {
  const Limit unbounded{.offset = 0, .count = std::nullopt};
  const Limit first_two{.offset = 0, .count = 2};
  const Limit skip_two{.offset = 2, .count = 3};
  const Limit skip_all{.offset = 10, .count = std::nullopt};
  return {
      {Keys("", false, "zzzzzzz", false),
       {"key0", "key2", "key4", "key5"},
       {"key5", "key4", "key2", "key0"},
       4},
      {Keys("key2", true, "key5", false),
       {"key4", "key5"},
       {"key5", "key4"},
       2},
      {Keys("key1", true, "key5", false),
       {"key2", "key4", "key5"},
       {"key5", "key4", "key2"},
       3},
      {Keys("key2", false, "key5", true),
       {"key2", "key4"},
       {"key4", "key2"},
       2},
      {Keys("key1", false, "key6", true),
       {"key2", "key4", "key5"},
       {"key5", "key4", "key2"},
       3},
      {Keys("key2", false, "key5", false, unbounded),
       {"key2", "key4", "key5"},
       {"key5", "key4", "key2"},
       3},
      {Keys("", false, "zzzzzzz", false, first_two),
       {"key0", "key2"},
       {"key5", "key4"},
       4},
      {Keys("", false, "zzzzzzz", false, skip_two),
       {"key4", "key5"},
       {"key2", "key0"},
       4},
      {Keys("", false, "zzzzzzz", false, skip_all), {}, {}, 4},
      {Keys("zzzzzzz", false, "", false), {}, {}, 0},
      {Keys("key0", false, "key0", true), {}, {}, 0},
  };
}

// A (score, name) entry ordered by score, then name, that can also be
// searched by score alone, like the sorted-set skiplist.
struct ScoredEntry {
  double score{};
  std::string name;
  auto operator<=>(const ScoredEntry&) const = default;
};

struct ScoreOrder {
  std::partial_ordering operator()(const ScoredEntry& left,
                                   const ScoredEntry& right) const {
    return left <=> right;
  }
  std::partial_ordering operator()(const ScoredEntry& entry,
                                   double score) const {
    return entry.score <=> score;
  }
};

using ScoredSkiplist = Skiplist<ScoredEntry, ScoreOrder>;

std::unique_ptr<ScoredSkiplist> MakeScoredSkiplist() {
  auto skiplist = std::make_unique<ScoredSkiplist>(4);
  for (const auto& entry : {ScoredEntry{.score = 2, .name = "b"},
                            ScoredEntry{.score = 1, .name = "a"},
                            ScoredEntry{.score = 3, .name = "a"},
                            ScoredEntry{.score = 2, .name = "a"},
                            ScoredEntry{.score = 1, .name = "b"}}) {
    skiplist->Insert(entry);
  }
  return skiplist;
}

std::vector<std::string> Names(const std::vector<ScoredEntry>& entries) {
  std::vector<std::string> names;
  names.reserve(entries.size());
  for (const auto& entry : entries) {
    names.push_back(std::to_string(static_cast<int>(entry.score)) + entry.name);
  }
  return names;
}

ScoredSkiplist::KeyRange<double> Scores(double min, bool min_exclusive,
                                        double max, bool max_exclusive) {
  return {.min = min,
          .max = max,
          .min_exclusive = min_exclusive,
          .max_exclusive = max_exclusive,
          .limit = std::nullopt,
          .reverse = false};
}

// Reverses the usual string order.
struct DescendingOrder {
  std::strong_ordering operator()(const std::string& left,
                                  const std::string& right) const {
    return right <=> left;
  }
};
}  // namespace

TEST(SkiplistDeathTest, RejectsInvalidLevel) {
  EXPECT_DEATH(StringSkiplist(0), "Check failed");
  EXPECT_DEATH(StringSkiplist(33), "Check failed");
}

TEST(SkiplistTest, DestroysStoredKeys) {
  const auto first = std::make_shared<int>(1);
  const auto second = std::make_shared<int>(2);
  {
    Skiplist<std::shared_ptr<int>> skiplist(1);
    skiplist.Insert(first);
    skiplist.Insert(second);
    EXPECT_EQ(first.use_count(), 2);
    EXPECT_TRUE(skiplist.Delete(second));
    EXPECT_EQ(second.use_count(), 1);
  }
  EXPECT_EQ(first.use_count(), 1);
}

TEST(SkiplistTest, Insertion) {
  auto skiplist = std::make_unique<StringSkiplist>(4);
  ASSERT_EQ(skiplist->Insert("key1"), "key1");
  ASSERT_EQ(skiplist->Insert("key2"), "key2");
  ASSERT_EQ(skiplist->Insert("key0"), "key0");
  ASSERT_EQ(skiplist->Insert("key1"), "key1");
  ASSERT_EQ(skiplist->Size(), 3);

  ASSERT_TRUE(skiplist->Contains("key1"));
  ASSERT_TRUE(skiplist->Contains("key2"));
  ASSERT_TRUE(skiplist->Contains("key0"));
  ASSERT_FALSE(skiplist->Contains("key_not_exist"));
}

TEST(SkiplistTest, Deletion) {
  auto skiplist = MakeInsertedSkiplist();
  ASSERT_TRUE(skiplist->Delete("key1"));
  ASSERT_FALSE(skiplist->Contains("key1"));
  ASSERT_EQ(skiplist->Size(), 2);
  ASSERT_FALSE(skiplist->Delete("key1"));
  ASSERT_EQ(skiplist->Size(), 2);

  ASSERT_TRUE(skiplist->Delete("key2"));
  ASSERT_FALSE(skiplist->Contains("key2"));
  ASSERT_EQ(skiplist->Size(), 1);
  ASSERT_FALSE(skiplist->Delete("key2"));
  ASSERT_EQ(skiplist->Size(), 1);

  ASSERT_FALSE(skiplist->Delete("key_not_exist"));
}

TEST(SkiplistTest, ReusesListAfterClear) {
  auto skiplist = MakeInsertedSkiplist();

  skiplist->Clear();

  EXPECT_EQ(skiplist->Size(), 0);
  EXPECT_EQ(skiplist->Begin(), skiplist->End());
  EXPECT_EQ(skiplist->Insert("new-key"), "new-key");
  EXPECT_TRUE(skiplist->Contains("new-key"));
}

TEST(SkiplistTest, Update) {
  auto skiplist = std::make_unique<StringSkiplist>(4);
  ASSERT_EQ(skiplist->Insert("key0"), "key0");
  ASSERT_EQ(skiplist->Insert("key1"), "key1");
  ASSERT_EQ(skiplist->Insert("key2"), "key2");
  ASSERT_EQ(skiplist->Insert("key3"), "key3");
  ASSERT_EQ(skiplist->Size(), 4);

  // Stays in place: key5 still sorts after key2.
  ASSERT_TRUE(skiplist->Update("key3", [](std::string& key) { key = "key5"; }));
  ASSERT_EQ(skiplist->Size(), 4);
  ASSERT_FALSE(skiplist->Contains("key3"));
  ASSERT_TRUE(skiplist->Contains("key5"));

  // Moves: key4 now sorts after key2.
  ASSERT_TRUE(skiplist->Update("key1", [](std::string& key) { key = "key4"; }));
  ASSERT_EQ(skiplist->Size(), 4);
  ASSERT_FALSE(skiplist->Contains("key1"));
  ASSERT_TRUE(skiplist->Contains("key4"));
  EXPECT_EQ(skiplist->RangeByRank(Ranks(0, 3, false, false)),
            (std::vector<std::string>{"key0", "key2", "key4", "key5"}));

  ASSERT_FALSE(
      skiplist->Update("key_not_exist", [](std::string& key) { key = "x"; }));
  ASSERT_EQ(skiplist->Size(), 4);
}

TEST(SkiplistTest, FindKeyByRank) {
  auto skiplist = MakeRankedSkiplist();
  EXPECT_EQ(skiplist->FindKeyByRank(0), "key0");
  EXPECT_EQ(skiplist->FindKeyByRank(1), "key2");
  EXPECT_EQ(skiplist->FindKeyByRank(2), "key4");
  EXPECT_EQ(skiplist->FindKeyByRank(3), "key5");
  EXPECT_EQ(skiplist->FindKeyByRank(-1), "key5");
  EXPECT_EQ(skiplist->FindKeyByRank(-2), "key4");
  EXPECT_EQ(skiplist->FindKeyByRank(-4), "key0");
}

TEST(SkiplistTest, FindRankOfKey) {
  auto skiplist = MakeRankedSkiplist();
  EXPECT_EQ(skiplist->FindRankOfKey("key0"), 0);
  EXPECT_EQ(skiplist->FindRankOfKey("key2"), 1);
  EXPECT_EQ(skiplist->FindRankOfKey("key5"), 3);
  EXPECT_EQ(skiplist->FindRankOfKey("key_not_exist"), std::nullopt);
}

TEST(SkiplistTest, RangeByRank) {
  auto skiplist = MakeRankedSkiplist();
  for (const auto& test : RankRangeTestCases()) {
    EXPECT_EQ(skiplist->RangeByRank(test.range), test.keys);
    EXPECT_EQ(skiplist->RangeByRank(Reversed(test.range)), test.reversed_keys);
  }
}

TEST(SkiplistTest, ReverseRankRangeStopsAtFirstKey) {
  auto skiplist = MakeRankedSkiplist();
  EXPECT_EQ(skiplist->RangeByRank(Reversed(Ranks(0, 100, false, false))),
            (std::vector<std::string>{"key5", "key4", "key2", "key0"}));
}

TEST(SkiplistTest, RangeByKey) {
  auto skiplist = MakeRankedSkiplist();
  for (const auto& test : KeyRangeTestCases()) {
    EXPECT_EQ(skiplist->RangeByKey(test.range), test.keys);
    EXPECT_EQ(skiplist->RangeByKey(Reversed(test.range)), test.reversed_keys);
  }
}

TEST(SkiplistTest, Count) {
  auto skiplist = MakeRankedSkiplist();
  for (const auto& test : KeyRangeTestCases()) {
    EXPECT_EQ(skiplist->Count(test.range), test.count);
  }
}

TEST(SkiplistTest, RangesByPartialBoundIncludeOrExcludeAllTies) {
  auto skiplist = MakeScoredSkiplist();
  EXPECT_EQ(Names(skiplist->RangeByKey(Scores(1, false, 2, false))),
            (std::vector<std::string>{"1a", "1b", "2a", "2b"}));
  EXPECT_EQ(Names(skiplist->RangeByKey(Scores(1, true, 3, false))),
            (std::vector<std::string>{"2a", "2b", "3a"}));
  EXPECT_EQ(Names(skiplist->RangeByKey(Scores(1, false, 3, true))),
            (std::vector<std::string>{"1a", "1b", "2a", "2b"}));
  EXPECT_EQ(Names(skiplist->RangeByKey(Reversed(Scores(1, true, 3, true)))),
            (std::vector<std::string>{"2b", "2a"}));
  EXPECT_EQ(skiplist->Count(Scores(1, true, 3, false)), 3);
  EXPECT_EQ(skiplist->Count(Scores(2, true, 2, false)), 0);
  EXPECT_TRUE(skiplist->RangeByKey(Scores(4, false, 9, false)).empty());
}

TEST(SkiplistTest, UpdateRepositionsByNewKey) {
  auto skiplist = MakeScoredSkiplist();
  const ScoredEntry entry{.score = 1, .name = "a"};
  ASSERT_TRUE(skiplist->Update(entry, [](ScoredEntry& e) { e.score = 5; }));
  EXPECT_EQ(skiplist->FindRankOfKey(ScoredEntry{.score = 5, .name = "a"}), 4);
  EXPECT_FALSE(skiplist->Contains(entry));
  EXPECT_EQ(Names(skiplist->RangeByRank({.min = 0,
                                         .max = 4,
                                         .min_exclusive = false,
                                         .max_exclusive = false,
                                         .limit = std::nullopt,
                                         .reverse = false})),
            (std::vector<std::string>{"1b", "2a", "2b", "3a", "5a"}));
}

TEST(SkiplistTest, Iteration) {
  auto skiplist = MakeRankedSkiplist();
  auto it = skiplist->Last();
  ASSERT_TRUE(it.Valid());
  ASSERT_EQ(*it, "key5");
  it.Next();
  ASSERT_FALSE(it.Valid());

  it = skiplist->Last();
  it.Prev();
  ASSERT_EQ(*it, "key4");
  ++it;
  ASSERT_EQ(*it, "key5");

  it = skiplist->Begin();
  ASSERT_EQ(*it, "key0");
  it.Prev();
  ASSERT_FALSE(it.Valid());

  std::vector<std::string> visited;
  for (auto key = skiplist->Begin(); key != skiplist->End(); ++key) {
    visited.push_back(*key);
  }
  EXPECT_EQ(visited,
            (std::vector<std::string>{"key0", "key2", "key4", "key5"}));
}

TEST(CustomSkiplistTest, Insertion) {
  Skiplist<std::string, DescendingOrder> skiplist(4);
  ASSERT_EQ(skiplist.Insert("key1"), "key1");
  ASSERT_EQ(skiplist.Insert("key2"), "key2");
  ASSERT_EQ(skiplist.Insert("key0"), "key0");
  ASSERT_EQ(skiplist.Insert("key1"), "key1");
  ASSERT_EQ(skiplist.Size(), 3);

  ASSERT_TRUE(skiplist.Contains("key1"));
  ASSERT_TRUE(skiplist.Contains("key2"));
  ASSERT_TRUE(skiplist.Contains("key0"));
  ASSERT_FALSE(skiplist.Contains("key_not_exist"));
}

TEST(CustomSkiplistTest, OrdersByComparator) {
  Skiplist<std::string, DescendingOrder> skiplist(4);
  skiplist.Insert("key1");
  skiplist.Insert("key2");
  skiplist.Insert("key0");
  EXPECT_EQ(skiplist.FindKeyByRank(0), "key2");
  EXPECT_EQ(skiplist.FindKeyByRank(1), "key1");
  EXPECT_EQ(skiplist.FindKeyByRank(2), "key0");
}

TEST(SkiplistDeathTest, RejectsOutOfRangeRanks) {
  auto skiplist = MakeRankedSkiplist();
  const auto size = static_cast<int64_t>(skiplist->Size());

  EXPECT_DEATH(skiplist->FindKeyByRank(size), "Check failed");
  EXPECT_DEATH(skiplist->FindKeyByRank(-size - 1), "Check failed");
  EXPECT_DEATH(skiplist->FindKeyByRank(INT_MAX), "Check failed");
  EXPECT_DEATH(skiplist->FindKeyByRank(INT_MIN), "Check failed");
}
}  // namespace redis_simple::in_memory
