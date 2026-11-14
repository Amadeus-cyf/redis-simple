#include "containers/quicklist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::containers {
TEST(QuicklistTest, PushAndPopPreserveDequeOrder) {
  Quicklist quicklist;

  ASSERT_TRUE(quicklist.LPush("b"));
  ASSERT_TRUE(quicklist.LPush("a"));
  ASSERT_TRUE(quicklist.RPush("c"));
  ASSERT_TRUE(quicklist.RPush("d"));

  EXPECT_EQ(quicklist.Size(), 4);
  EXPECT_EQ(quicklist.NodeCount(), 1);
  EXPECT_EQ(quicklist.LPop(), "a");
  EXPECT_EQ(quicklist.RPop(), "d");
  EXPECT_EQ(quicklist.LPop(), "b");
  EXPECT_EQ(quicklist.RPop(), "c");
  EXPECT_EQ(quicklist.LPop(), std::nullopt);
  EXPECT_TRUE(quicklist.Empty());
  EXPECT_EQ(quicklist.NodeCount(), 0);
}

TEST(QuicklistTest, SplitTailOnLimit) {
  Quicklist quicklist(48);
  const std::string value(32, 'x');

  ASSERT_TRUE(quicklist.RPush(value + "1"));
  ASSERT_TRUE(quicklist.RPush(value + "2"));
  ASSERT_TRUE(quicklist.RPush(value + "3"));

  EXPECT_EQ(quicklist.Size(), 3);
  EXPECT_EQ(quicklist.NodeCount(), 3);
  EXPECT_EQ(quicklist.LPop(), value + "1");
  EXPECT_EQ(quicklist.LPop(), value + "2");
  EXPECT_EQ(quicklist.LPop(), value + "3");
  EXPECT_EQ(quicklist.NodeCount(), 0);
}

TEST(QuicklistTest, SplitHeadOnLimit) {
  Quicklist quicklist(48);
  const std::string value(32, 'y');

  ASSERT_TRUE(quicklist.LPush(value + "1"));
  ASSERT_TRUE(quicklist.LPush(value + "2"));
  ASSERT_TRUE(quicklist.LPush(value + "3"));

  EXPECT_EQ(quicklist.Size(), 3);
  EXPECT_EQ(quicklist.NodeCount(), 3);
  EXPECT_EQ(quicklist.RPop(), value + "1");
  EXPECT_EQ(quicklist.RPop(), value + "2");
  EXPECT_EQ(quicklist.RPop(), value + "3");
  EXPECT_EQ(quicklist.NodeCount(), 0);
}

TEST(QuicklistTest, AllowLargeSingleEntry) {
  Quicklist quicklist(16);
  const std::string value(128, 'z');

  ASSERT_TRUE(quicklist.RPush(value));

  EXPECT_EQ(quicklist.Size(), 1);
  EXPECT_EQ(quicklist.NodeCount(), 1);
  EXPECT_EQ(quicklist.RPop(), value);
  EXPECT_TRUE(quicklist.Empty());
}

TEST(QuicklistTest, MergeAfterLPop) {
  Quicklist quicklist(96);
  const std::string value(32, 'm');

  ASSERT_TRUE(quicklist.RPush(value + "1"));
  ASSERT_TRUE(quicklist.RPush(value + "2"));
  ASSERT_TRUE(quicklist.RPush(value + "3"));
  ASSERT_EQ(quicklist.NodeCount(), 2);

  EXPECT_EQ(quicklist.LPop(), value + "1");

  EXPECT_EQ(quicklist.NodeCount(), 1);
  EXPECT_EQ(quicklist.Range(0, 1),
            (std::vector<std::string>{value + "2", value + "3"}));
}

TEST(QuicklistTest, MergeAfterRPop) {
  Quicklist quicklist(96);
  const std::string value(32, 'n');

  ASSERT_TRUE(quicklist.LPush(value + "3"));
  ASSERT_TRUE(quicklist.LPush(value + "2"));
  ASSERT_TRUE(quicklist.LPush(value + "1"));
  ASSERT_EQ(quicklist.NodeCount(), 2);

  EXPECT_EQ(quicklist.RPop(), value + "3");

  EXPECT_EQ(quicklist.NodeCount(), 1);
  EXPECT_EQ(quicklist.Range(0, 1),
            (std::vector<std::string>{value + "1", value + "2"}));
}

TEST(QuicklistTest, RangeAcrossNodes) {
  Quicklist quicklist(48);
  const std::string value(32, 'r');

  ASSERT_TRUE(quicklist.RPush(value + "1"));
  ASSERT_TRUE(quicklist.RPush(value + "2"));
  ASSERT_TRUE(quicklist.RPush(value + "3"));
  ASSERT_TRUE(quicklist.RPush(value + "4"));
  ASSERT_EQ(quicklist.NodeCount(), 4);

  EXPECT_EQ(quicklist.Range(0, 3),
            (std::vector<std::string>{value + "1", value + "2", value + "3",
                                      value + "4"}));
  EXPECT_EQ(quicklist.Range(1, 2),
            (std::vector<std::string>{value + "2", value + "3"}));
  EXPECT_EQ(quicklist.Range(2, 100),
            (std::vector<std::string>{value + "3", value + "4"}));
}

TEST(QuicklistTest, RangeInvalidBounds) {
  Quicklist quicklist(48);

  EXPECT_TRUE(quicklist.Range(0, 0).empty());

  ASSERT_TRUE(quicklist.RPush("one"));
  ASSERT_TRUE(quicklist.RPush("two"));

  EXPECT_TRUE(quicklist.Range(2, 3).empty());
  EXPECT_TRUE(quicklist.Range(1, 0).empty());
}

TEST(QuicklistTest, SetSplitsOversizedNode) {
  Quicklist quicklist(96);
  const std::string value(32, 's');
  const std::string large_value(128, 'l');

  ASSERT_TRUE(quicklist.RPush(value + "1"));
  ASSERT_TRUE(quicklist.RPush(value + "2"));
  ASSERT_EQ(quicklist.NodeCount(), 1);

  ASSERT_TRUE(quicklist.Set(0, large_value));

  EXPECT_EQ(quicklist.NodeCount(), 2);
  EXPECT_EQ(quicklist.Range(0, 1),
            (std::vector<std::string>{large_value, value + "2"}));
  EXPECT_FALSE(quicklist.Set(2, "missing"));
}

TEST(QuicklistTest, RemoveAcrossNodes) {
  Quicklist quicklist(48);
  const std::string value(32, 'd');

  ASSERT_TRUE(quicklist.RPush(value + "1"));
  ASSERT_TRUE(quicklist.RPush(value + "2"));
  ASSERT_TRUE(quicklist.RPush(value + "3"));
  ASSERT_TRUE(quicklist.RPush(value + "2"));
  ASSERT_TRUE(quicklist.RPush(value + "4"));
  ASSERT_EQ(quicklist.NodeCount(), 5);

  ASSERT_EQ(
      quicklist.Remove(value + "2", 1, Quicklist::RemoveDirection::kFromTail),
      1);
  EXPECT_EQ(quicklist.Range(0, 3),
            (std::vector<std::string>{value + "1", value + "2", value + "3",
                                      value + "4"}));

  ASSERT_EQ(
      quicklist.Remove(value + "2", 0, Quicklist::RemoveDirection::kFromHead),
      1);
  EXPECT_EQ(quicklist.Range(0, 2),
            (std::vector<std::string>{value + "1", value + "3", value + "4"}));
}

TEST(QuicklistTest, TrimAcrossNodes) {
  Quicklist quicklist(48);
  const std::string value(32, 't');

  ASSERT_TRUE(quicklist.RPush(value + "1"));
  ASSERT_TRUE(quicklist.RPush(value + "2"));
  ASSERT_TRUE(quicklist.RPush(value + "3"));
  ASSERT_TRUE(quicklist.RPush(value + "4"));
  ASSERT_TRUE(quicklist.RPush(value + "5"));
  ASSERT_EQ(quicklist.NodeCount(), 5);

  ASSERT_TRUE(quicklist.Trim(1, 3));
  EXPECT_EQ(quicklist.Range(0, 2),
            (std::vector<std::string>{value + "2", value + "3", value + "4"}));

  ASSERT_TRUE(quicklist.Trim(1, 0));
  EXPECT_TRUE(quicklist.Empty());
  EXPECT_EQ(quicklist.NodeCount(), 0);
}

TEST(QuicklistTest, RemoveFromTailWithinOneNode) {
  Quicklist quicklist(256);
  for (std::string_view value : {"x", "keep", "x", "x", "keep", "x"}) {
    ASSERT_TRUE(quicklist.RPush(value));
  }
  ASSERT_EQ(quicklist.NodeCount(), 1);

  EXPECT_EQ(quicklist.Remove("x", 2, Quicklist::RemoveDirection::kFromTail), 2);
  EXPECT_EQ(quicklist.Range(0, quicklist.Size() - 1),
            (std::vector<std::string>{"x", "keep", "x", "keep"}));
}

TEST(QuicklistTest, ReleasesSingleListpackWithoutRebuilding) {
  Quicklist quicklist(256);
  ASSERT_TRUE(quicklist.RPush("one"));
  ASSERT_TRUE(quicklist.RPush("two"));
  ASSERT_EQ(quicklist.NodeCount(), 1);

  std::unique_ptr<Listpack> listpack = quicklist.ReleaseListpack();

  ASSERT_NE(listpack, nullptr);
  EXPECT_TRUE(quicklist.Empty());
  EXPECT_EQ(quicklist.NodeCount(), 0);
  ASSERT_EQ(listpack->Size(), 2);
  const std::optional<size_t> first = listpack->First();
  const std::optional<size_t> last = listpack->Last();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(listpack->Get(first.value_or(0)), "one");
  EXPECT_EQ(listpack->Get(last.value_or(0)), "two");
}

TEST(QuicklistTest, RangesFromEitherEndMatchAModel) {
  // Small nodes give a long chain, so ranges near the tail are reached by
  // walking from the tail and ranges near the head from the head.
  Quicklist quicklist(48);
  std::vector<std::string> model;
  model.reserve(40);
  for (int index = 0; index < 40; ++index) {
    model.push_back("value:" + std::to_string(index));
    ASSERT_TRUE(quicklist.RPush(model.back()));
  }
  ASSERT_GT(quicklist.NodeCount(), 8);

  for (size_t start = 0; start < model.size(); ++start) {
    for (size_t stop = start; stop < model.size() + 2; ++stop) {
      const size_t last = std::min(stop, model.size() - 1);
      const std::vector<std::string> expected(
          model.begin() + static_cast<std::ptrdiff_t>(start),
          model.begin() + static_cast<std::ptrdiff_t>(last) + 1);
      std::vector<std::string> forward;
      ASSERT_TRUE(quicklist.ForEach(start, stop, [&](std::string_view value) {
        forward.emplace_back(value);
        return true;
      }));
      EXPECT_EQ(forward, expected) << start << ".." << stop;

      std::vector<std::string> reverse;
      ASSERT_TRUE(
          quicklist.ForEachReverse(start, stop, [&](std::string_view value) {
            reverse.emplace_back(value);
            return true;
          }));
      EXPECT_EQ(reverse,
                std::vector<std::string>(expected.rbegin(), expected.rend()))
          << start << ".." << stop;
    }
  }

  // A visitor returning false stops both walks after one call.
  size_t calls = 0;
  const auto stop_now = [&calls](std::string_view) {
    ++calls;
    return false;
  };
  EXPECT_FALSE(quicklist.ForEach(30, 39, stop_now));
  EXPECT_FALSE(quicklist.ForEachReverse(0, 9, stop_now));
  EXPECT_EQ(calls, 2);
}

TEST(QuicklistTest, SetSplitsOnlyTheReplacedNode) {
  Quicklist quicklist(96);
  const std::string value(30, 'v');
  for (int index = 0; index < 9; ++index) {
    ASSERT_TRUE(quicklist.RPush(value + std::to_string(index)));
  }
  const size_t nodes = quicklist.NodeCount();
  ASSERT_GE(nodes, 3);

  // Replacing a middle element with an oversized value splits its node; the
  // nodes after it are left as they were.
  const std::string large(200, 'L');
  ASSERT_TRUE(quicklist.Set(4, large));
  std::vector<std::string> expected;
  expected.reserve(9);
  for (int index = 0; index < 9; ++index) {
    expected.push_back(index == 4 ? large : value + std::to_string(index));
  }
  EXPECT_EQ(quicklist.Range(0, 8), expected);
  EXPECT_GT(quicklist.NodeCount(), nodes);

  // The same at the tail, where the replaced node has no successor.
  ASSERT_TRUE(quicklist.Set(8, large));
  expected[8] = large;
  EXPECT_EQ(quicklist.Range(0, 8), expected);
}

TEST(QuicklistTest, FreesAVeryLongChainOfNodes) {
  // The smallest node size gives every element a node of its own. Freeing
  // the nodes one inside another would need a stack frame for each.
  constexpr size_t kNodes = 300'000;
  {
    Quicklist quicklist(1);
    for (size_t index = 0; index < kNodes; ++index) {
      ASSERT_TRUE(quicklist.RPush("element"));
    }
    ASSERT_EQ(quicklist.NodeCount(), kNodes);
    // Trimming to nothing clears the list; destruction frees the rest.
    ASSERT_TRUE(quicklist.Trim(1, 0));
    EXPECT_EQ(quicklist.NodeCount(), 0);
    for (size_t index = 0; index < kNodes; ++index) {
      ASSERT_TRUE(quicklist.RPush("element"));
    }
  }
}
}  // namespace redis_simple::containers
