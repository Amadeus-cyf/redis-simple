#include "data_types/set/set_algebra.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "data_types/set/set.h"

namespace redis_simple::data_types {
namespace {
using Members = std::set<std::string>;

// Members prefix + first, ..., prefix + (last - 1). An empty prefix gives an
// intset; a prefix gives a listpack up to Set::kListpackMaxEntries members
// and a dict past it.
std::unique_ptr<Set> RangeSet(std::string_view prefix, size_t first,
                              size_t last) {
  auto set = Set::Create();
  for (size_t i = first; i < last; ++i) {
    set->Add(std::string(prefix) + std::to_string(i));
  }
  return set;
}

Members Range(std::string_view prefix, size_t first, size_t last) {
  Members members;
  for (size_t i = first; i < last; ++i) {
    members.insert(std::string(prefix) + std::to_string(i));
  }
  return members;
}

std::vector<const Set*> Pointers(
    const std::vector<std::unique_ptr<Set>>& sets) {
  std::vector<const Set*> pointers;
  pointers.reserve(sets.size());
  for (const auto& set : sets) {
    pointers.push_back(set.get());
  }
  return pointers;
}

Members Common(const std::vector<std::unique_ptr<Set>>& sets) {
  std::vector<const Set*> pointers = Pointers(sets);
  Members members;
  ForEachCommonMember(std::span(pointers), [&members](std::string_view member) {
    EXPECT_TRUE(members.emplace(member).second) << member;
  });
  return members;
}

enum class DifferenceAlgorithm : uint8_t { kChosen, kProbing, kSubtracting };

Members Difference(const Set& first,
                   const std::vector<std::unique_ptr<Set>>& others,
                   DifferenceAlgorithm algorithm) {
  std::vector<const Set*> pointers = Pointers(others);
  Members members;
  const auto collect = [&members](std::string_view member) {
    EXPECT_TRUE(members.emplace(member).second) << member;
  };
  switch (algorithm) {
    case DifferenceAlgorithm::kChosen:
      ForEachDifferenceMember(first, std::span(pointers), collect);
      break;
    case DifferenceAlgorithm::kProbing:
      ForEachDifferenceMemberByProbing(first, std::span(pointers), collect);
      break;
    case DifferenceAlgorithm::kSubtracting:
      ForEachDifferenceMemberBySubtracting(first, std::span(pointers), collect);
      break;
  }
  return members;
}

constexpr std::array kEveryAlgorithm = {DifferenceAlgorithm::kChosen,
                                        DifferenceAlgorithm::kProbing,
                                        DifferenceAlgorithm::kSubtracting};
}  // namespace

TEST(SetAlgebraTest, CommonMembersDoNotDependOnArgumentOrder) {
  for (const std::string_view prefix : {"", "member:"}) {
    std::vector<std::unique_ptr<Set>> sets;
    sets.push_back(RangeSet(prefix, 0, 1'000));
    sets.push_back(RangeSet(prefix, 990, 1'010));
    sets.push_back(RangeSet(prefix, 500, 1'000));
    EXPECT_EQ(Common(sets), Range(prefix, 990, 1'000));
    std::ranges::reverse(sets);
    EXPECT_EQ(Common(sets), Range(prefix, 990, 1'000));
  }
}

TEST(SetAlgebraTest, CommonMembersOfOneSetAreItsMembers) {
  std::vector<std::unique_ptr<Set>> sets;
  sets.push_back(RangeSet("member:", 0, 200));
  EXPECT_EQ(Common(sets), Range("member:", 0, 200));
}

TEST(SetAlgebraTest, CommonMembersOfASetAndItselfAreItsMembers) {
  const auto set = RangeSet("member:", 0, 200);
  std::vector<const Set*> sets{set.get(), set.get()};
  size_t count = 0;
  ForEachCommonMember(std::span(sets), [&count](std::string_view) { ++count; });
  EXPECT_EQ(count, 200);
}

TEST(SetAlgebraTest, DisjointSetsHaveNoCommonMembers) {
  std::vector<std::unique_ptr<Set>> sets;
  sets.push_back(RangeSet("member:", 0, 300));
  sets.push_back(RangeSet("member:", 300, 310));
  EXPECT_TRUE(Common(sets).empty());
}

TEST(SetAlgebraTest, EveryDifferenceAlgorithmGivesTheSameMembers) {
  for (const std::string_view prefix : {"", "member:"}) {
    const auto first = RangeSet(prefix, 0, 1'000);
    std::vector<std::unique_ptr<Set>> others;
    others.push_back(RangeSet(prefix, 0, 10));
    others.push_back(RangeSet(prefix, 400, 2'000));
    others.push_back(RangeSet(prefix, 5, 100));
    for (const DifferenceAlgorithm algorithm : kEveryAlgorithm) {
      EXPECT_EQ(Difference(*first, others, algorithm), Range(prefix, 100, 400));
    }
  }
}

TEST(SetAlgebraTest, DifferenceWithNoOtherSetIsTheFirstSet) {
  const auto first = RangeSet("member:", 0, 200);
  for (const DifferenceAlgorithm algorithm : kEveryAlgorithm) {
    EXPECT_EQ(Difference(*first, {}, algorithm), Range("member:", 0, 200));
  }
}

TEST(SetAlgebraTest, DifferenceIsEmptyOnceEveryMemberIsRemoved) {
  const auto first = RangeSet("member:", 0, 200);
  std::vector<std::unique_ptr<Set>> others;
  others.push_back(RangeSet("member:", 0, 150));
  others.push_back(RangeSet("member:", 100, 400));
  others.push_back(RangeSet("member:", 0, 5));
  for (const DifferenceAlgorithm algorithm : kEveryAlgorithm) {
    EXPECT_TRUE(Difference(*first, others, algorithm).empty());
  }
}

TEST(SetAlgebraTest, DifferenceOfASetAndItselfIsEmpty) {
  const auto set = RangeSet("member:", 0, 200);
  std::vector<const Set*> others{set.get()};
  size_t count = 0;
  ForEachDifferenceMember(*set, std::span(others),
                          [&count](std::string_view) { ++count; });
  EXPECT_EQ(count, 0);
}

TEST(SetAlgebraTest, ProbingIsChosenUnlessTheOtherSetsAreManyAndSmall) {
  const auto large = RangeSet("member:", 0, 1'000);
  const auto small = RangeSet("member:", 0, 10);
  const std::vector<const Set*> one_small{small.get()};
  const std::vector<const Set*> three_small(3, small.get());
  const std::vector<const Set*> four_small(4, small.get());
  const std::vector<const Set*> four_large(4, large.get());
  EXPECT_TRUE(DifferenceProbingIsCheaper(*large, {}));
  EXPECT_TRUE(DifferenceProbingIsCheaper(*large, one_small));
  EXPECT_TRUE(DifferenceProbingIsCheaper(*large, three_small));
  EXPECT_FALSE(DifferenceProbingIsCheaper(*large, four_small));
  EXPECT_TRUE(DifferenceProbingIsCheaper(*large, four_large));
  EXPECT_TRUE(DifferenceProbingIsCheaper(*small, four_large));
}
}  // namespace redis_simple::data_types
