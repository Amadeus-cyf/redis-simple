#include "memory/skiplist.h"

#include <gtest/gtest.h>

#include <climits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "logging/logger.h"

namespace redis_simple::in_memory {
struct RangeByRankSpecTestCase {
  RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec spec,
                          std::vector<std::string> keys,
                          std::vector<std::string> revkeys)
      : spec(spec), keys(std::move(keys)), revkeys(std::move(revkeys)) {}
  Skiplist<std::string>::SkiplistRangeByRankSpec spec;
  std::vector<std::string> keys;
  std::vector<std::string> revkeys;
};

struct RangeByKeySpecTestCase {
  RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec spec,
                         std::vector<std::string> keys,
                         std::vector<std::string> revkeys, size_t count)
      : spec(std::move(spec)),
        keys(std::move(keys)),
        revkeys(std::move(revkeys)),
        count(count) {}
  Skiplist<std::string>::SkiplistRangeByKeySpec spec;
  std::vector<std::string> keys;
  std::vector<std::string> revkeys;
  // The value used for testing Skiplist.Count(), the result is not related to
  // SkiplistLimitSpec
  size_t count;
};

std::vector<RangeByRankSpecTestCase> RangeByRankSpecTestCases();
std::vector<RangeByKeySpecTestCase> RangeByKeySpecTestCases();
void ScanSkiplist(const Skiplist<std::string>* skiplist);

namespace {
std::unique_ptr<Skiplist<std::string>> MakeInsertedSkiplist() {
  auto skiplist = std::make_unique<Skiplist<std::string>>(4);
  skiplist->Insert("key1");
  skiplist->Insert("key2");
  skiplist->Insert("key0");
  return skiplist;
}

std::unique_ptr<Skiplist<std::string>> MakeRankedSkiplist() {
  auto skiplist = std::make_unique<Skiplist<std::string>>(4);
  skiplist->Insert("key0");
  skiplist->Insert("key2");
  skiplist->Insert("key4");
  skiplist->Insert("key5");
  return skiplist;
}

struct CountingDestructor {
  size_t* count;

  void operator()(const std::string& /*key*/) const { ++*count; }
};
}  // namespace

TEST(SkiplistTest, RejectsInvalidLevel) {
  EXPECT_DEATH((Skiplist<std::string>(0)), "Check failed");
  EXPECT_DEATH((Skiplist<std::string>(33)), "Check failed");
}

TEST(SkiplistTest, DestroysOnlyStoredKeys) {
  size_t destroy_count = 0;
  using CountingSkiplist =
      Skiplist<std::string, decltype(kDefaultCompare<std::string>),
               CountingDestructor>;
  {
    CountingSkiplist skiplist(1, kDefaultCompare<std::string>,
                              CountingDestructor{&destroy_count});
    skiplist.Insert("one");
    skiplist.Insert("two");
  }
  EXPECT_EQ(destroy_count, 2);
}

TEST(SkiplistTest, Insertion) {
  auto skiplist = std::make_unique<Skiplist<std::string>>(4);
  ASSERT_EQ(skiplist->Insert("key1"), "key1");
  ASSERT_EQ(skiplist->Insert("key2"), "key2");
  ASSERT_EQ(skiplist->Insert("key0"), "key0");
  ASSERT_EQ(skiplist->Insert("key1"), "key1");
  ASSERT_EQ(skiplist->Size(), 3);

  ASSERT_TRUE(skiplist->Contains("key1"));
  ASSERT_TRUE(skiplist->Contains("key2"));
  ASSERT_TRUE(skiplist->Contains("key0"));
  ASSERT_TRUE(!skiplist->Contains("key_not_exist"));
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
  auto skiplist = std::make_unique<Skiplist<std::string>>(4);
  ASSERT_EQ(skiplist->Insert("key0"), "key0");
  ASSERT_EQ("key1", skiplist->Insert("key1"));
  ASSERT_EQ("key2", skiplist->Insert("key2"));
  ASSERT_EQ("key3", skiplist->Insert("key3"));
  ASSERT_EQ(skiplist->Size(), 4);

  ASSERT_TRUE(skiplist->Update("key3", "key5"));
  ASSERT_EQ(skiplist->Size(), 4);
  ASSERT_FALSE(skiplist->Contains("key3"));
  ASSERT_TRUE(skiplist->Contains("key5"));

  ASSERT_TRUE(skiplist->Update("key1", "key4"));

  ASSERT_EQ(skiplist->Size(), 4);
  ASSERT_FALSE(skiplist->Contains("key1"));
  ASSERT_TRUE(skiplist->Contains("key4"));

  ASSERT_TRUE(!skiplist->Update("key_not_exist", "key6"));
  ASSERT_EQ(skiplist->Size(), 4);
}

TEST(SkiplistTest, FindKeyByRank) {
  auto skiplist = MakeRankedSkiplist();
  const std::string& s0 = skiplist->FindKeyByRank(0);
  ASSERT_EQ(s0, "key0");

  const std::string& s1 = skiplist->FindKeyByRank(1);
  ASSERT_EQ(s1, "key2");

  const std::string& s2 = skiplist->FindKeyByRank(-1);
  ASSERT_EQ(s2, "key5");

  const std::string& s3 = skiplist->FindKeyByRank(-2);
  ASSERT_EQ(s3, "key4");

  const std::string& s4 = skiplist->FindKeyByRank(-4);
  ASSERT_EQ(s4, "key0");

  EXPECT_DEATH(skiplist->FindKeyByRank(skiplist->Size()), "Check failed");
  EXPECT_DEATH(skiplist->FindKeyByRank(-skiplist->Size() - 1), "Check failed");
  EXPECT_DEATH(skiplist->FindKeyByRank(INT_MAX), "Check failed");
  EXPECT_DEATH(skiplist->FindKeyByRank(INT_MIN), "Check failed");
}

TEST(SkiplistTest, FindRankOfKey) {
  auto skiplist = MakeRankedSkiplist();
  EXPECT_EQ(skiplist->FindRankOfKey("key0"), 0);

  EXPECT_EQ(skiplist->FindRankOfKey("key2"), 1);

  EXPECT_EQ(skiplist->FindRankOfKey("key5"), 3);

  EXPECT_EQ(skiplist->FindRankOfKey("key_not_exist"), std::nullopt);
}

TEST(SkiplistTest, ArrayAccess) {
  auto skiplist = MakeRankedSkiplist();
  ASSERT_EQ((*skiplist)[0], "key0");
  ASSERT_EQ((*skiplist)[1], "key2");
  ASSERT_EQ((*skiplist)[2], "key4");
  ASSERT_EQ((*skiplist)[3], "key5");
  ASSERT_EQ((*skiplist)[skiplist->Size() - 1], "key5");

  EXPECT_DEATH((*skiplist)[skiplist->Size()], "Check failed");
  EXPECT_DEATH((*skiplist)[-1], "Check failed");
  EXPECT_DEATH((*skiplist)[INT_MAX], "Check failed");
  EXPECT_DEATH((*skiplist)[INT_MIN], "Check failed");
}

TEST(SkiplistTest, RangeByRank) {
  auto skiplist = MakeRankedSkiplist();
  const auto tests = RangeByRankSpecTestCases();
  for (const auto& test : tests) {
    ASSERT_EQ(skiplist->RangeByRank(&test.spec), test.keys);
  }
}

TEST(SkiplistTest, RevRangeByRank) {
  auto skiplist = MakeRankedSkiplist();
  const auto tests = RangeByRankSpecTestCases();
  for (const auto& test : tests) {
    ASSERT_EQ(skiplist->RevRangeByRank(&test.spec), test.revkeys);
  }
}

TEST(SkiplistTest, RangeByKey) {
  auto skiplist = MakeRankedSkiplist();
  const auto tests = RangeByKeySpecTestCases();
  for (const auto& test : tests) {
    ASSERT_EQ(skiplist->RangeByKey(&test.spec), test.keys);
  }
}

TEST(SkiplistTest, RevRangeByKey) {
  auto skiplist = MakeRankedSkiplist();
  const auto tests = RangeByKeySpecTestCases();
  for (const auto& test : tests) {
    ASSERT_EQ(skiplist->RevRangeByKey(&test.spec), test.revkeys);
  }
}

TEST(SkiplistTest, Count) {
  auto skiplist = MakeRankedSkiplist();
  const auto tests = RangeByKeySpecTestCases();
  for (const auto& test : tests) {
    ASSERT_EQ(skiplist->Count(&test.spec), test.count);
  }
}

std::vector<RangeByRankSpecTestCase> RangeByRankSpecTestCases() {
  static const Skiplist<std::string>::SkiplistLimitSpec limit1(0, std::nullopt);
  static const Skiplist<std::string>::SkiplistLimitSpec limit2(0, 2);
  static const Skiplist<std::string>::SkiplistLimitSpec limit3(2, 3);
  static const Skiplist<std::string>::SkiplistLimitSpec limit4(10,
                                                               std::nullopt);

  return {
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  0, 3, false, false, nullptr),
                              {"key0", "key2", "key4", "key5"},
                              {"key5", "key4", "key2", "key0"}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  1, 3, true, false, nullptr),
                              {"key4", "key5"}, {"key2", "key0"}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  1, 3, false, true, nullptr),
                              {"key2", "key4"}, {"key4", "key2"}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  1, 3, false, false, &limit1),
                              {"key2", "key4", "key5"},
                              {"key4", "key2", "key0"}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  1, 3, false, false, &limit2),
                              {"key2", "key4"}, {"key4", "key2"}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  0, 3, false, false, &limit3),
                              {"key4", "key5"}, {"key2", "key0"}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  0, 4, false, false, &limit4),
                              {}, {}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  2, 1, false, false, nullptr),
                              {}, {}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  1, 1, true, false, nullptr),
                              {}, {}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  1, 1, false, true, nullptr),
                              {}, {}),
      RangeByRankSpecTestCase(Skiplist<std::string>::SkiplistRangeByRankSpec(
                                  100000, 1000000, false, false, nullptr),
                              {}, {})};
}

std::vector<RangeByKeySpecTestCase> RangeByKeySpecTestCases() {
  static const Skiplist<std::string>::SkiplistLimitSpec limit1(0, std::nullopt);
  static const Skiplist<std::string>::SkiplistLimitSpec limit2(0, 2);
  static const Skiplist<std::string>::SkiplistLimitSpec limit3(2, 3);
  static const Skiplist<std::string>::SkiplistLimitSpec limit4(10,
                                                               std::nullopt);

  return {
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "", false, "zzzzzzz", false, nullptr),
                             {"key0", "key2", "key4", "key5"},
                             {"key5", "key4", "key2", "key0"}, 4),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "key2", true, "key5", false, nullptr),
                             {"key4", "key5"}, {"key5", "key4"}, 2),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "key1", true, "key5", false, nullptr),
                             {"key2", "key4", "key5"}, {"key5", "key4", "key2"},
                             3),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "key2", false, "key5", true, nullptr),
                             {"key2", "key4"}, {"key4", "key2"}, 2),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "key1", false, "key6", true, nullptr),
                             {"key2", "key4", "key5"}, {"key5", "key4", "key2"},
                             3),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "key2", false, "key5", false, &limit1),
                             {"key2", "key4", "key5"}, {"key5", "key4", "key2"},
                             3),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "", false, "zzzzzzz", false, &limit2),
                             {"key0", "key2"}, {"key5", "key4"}, 4),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "", false, "zzzzzzz", false, &limit3),
                             {"key4", "key5"}, {"key2", "key0"}, 4),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "", false, "zzzzzzz", false, &limit4),
                             {}, {}, 4),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "zzzzzzz", false, "", false, nullptr),
                             {}, {}, 0),
      RangeByKeySpecTestCase(Skiplist<std::string>::SkiplistRangeByKeySpec(
                                 "key0", false, "key0", true, nullptr),
                             {}, {}, 0),
  };
}

TEST(SkiplistTest, Iteration) {
  auto skiplist = MakeRankedSkiplist();
  auto it = Skiplist<std::string>::Iterator(skiplist.get());
  it.SeekToLast();
  ASSERT_EQ(*it, "key5");
  ASSERT_TRUE(it.Valid());
  it.Next();
  ASSERT_FALSE(it.Valid());

  it.SeekToLast();
  it.Prev();
  ASSERT_EQ(*it, "key4");

  ++it;
  ASSERT_EQ(*it, "key5");

  it.SeekToFirst();
  ASSERT_EQ(*it, "key0");
  ASSERT_TRUE(it.Valid());

  ++it;
  ASSERT_EQ(*it, "key2");
  ASSERT_TRUE(it.Valid());

  ScanSkiplist(skiplist.get());
}

void ScanSkiplist(const Skiplist<std::string>* skiplist) {
  RS_LOG_DEBUG("----start scanning skiplist----\n");
  for (auto it = skiplist->Begin(); it != skiplist->End(); ++it) {
    RS_LOG_DEBUG("%s\n", (*it).c_str());
  }
  RS_LOG_DEBUG("----end----\n");

  skiplist->Print();
}

struct Comparator {
  int operator()(const std::string& k1, const std::string& k2) const {
    if (k1 < k2) {
      return 1;
    }
    if (k1 == k2) {
      return 0;
    }
    return -1;
  }
};

namespace {
std::unique_ptr<Skiplist<std::string, Comparator>> MakeCustomSkiplist() {
  Comparator cmp;
  return std::make_unique<Skiplist<std::string, Comparator>>(4, cmp);
}
}  // namespace

TEST(CustomSkiplistTest, Insertion) {
  auto skiplist = MakeCustomSkiplist();
  ASSERT_EQ(skiplist->Insert("key1"), "key1");
  ASSERT_EQ(skiplist->Insert("key2"), "key2");
  ASSERT_EQ(skiplist->Insert("key0"), "key0");
  ASSERT_EQ(skiplist->Insert("key1"), "key1");
  ASSERT_EQ(skiplist->Size(), 3);

  ASSERT_TRUE(skiplist->Contains("key1"));
  ASSERT_TRUE(skiplist->Contains("key2"));
  ASSERT_TRUE(skiplist->Contains("key0"));
  ASSERT_TRUE(!skiplist->Contains("key_not_exist"));
}

TEST(CustomSkiplistTest, ArrayAccess) {
  auto skiplist = MakeCustomSkiplist();
  skiplist->Insert("key1");
  skiplist->Insert("key2");
  skiplist->Insert("key0");
  ASSERT_EQ((*skiplist)[0], "key2");
  ASSERT_EQ((*skiplist)[1], "key1");
  ASSERT_EQ((*skiplist)[2], "key0");
  ASSERT_EQ((*skiplist)[skiplist->Size() - 1], "key0");

  EXPECT_DEATH((*skiplist)[skiplist->Size()], "Check failed");
  EXPECT_DEATH((*skiplist)[-1], "Check failed");
  EXPECT_DEATH((*skiplist)[INT_MAX], "Check failed");
  EXPECT_DEATH((*skiplist)[INT_MIN], "Check failed");
}
}  // namespace redis_simple::in_memory
