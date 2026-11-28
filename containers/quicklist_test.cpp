#include "containers/quicklist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fuzz/sequence_model.h"

namespace redis_simple::containers {
namespace {
// The size in bytes of each node's listpack, head to tail.
std::vector<size_t> NodeBytes(const Quicklist& quicklist) {
  std::vector<size_t> bytes;
  quicklist.ForEachNode([&bytes](const Listpack& node) {
    bytes.push_back(node.TotalBytes());
    return true;
  });
  return bytes;
}

// Whether some node and the one after it would fit in one node together.
bool HasNeighborsThatFitTogether(const Quicklist& quicklist,
                                 size_t node_max_bytes) {
  const std::vector<size_t> bytes = NodeBytes(quicklist);
  for (size_t index = 1; index < bytes.size(); ++index) {
    if (bytes[index - 1] + bytes[index] - Listpack::kEmptyListpackBytes <=
        node_max_bytes) {
      return true;
    }
  }
  return false;
}

// Checks what holds after every operation: no node is empty, only a node of
// one element exceeds `node_max_bytes`, and the nodes add up to the list.
void ExpectValidNodes(const Quicklist& quicklist, size_t node_max_bytes) {
  size_t nodes = 0;
  size_t elements = 0;
  quicklist.ForEachNode([&](const Listpack& node) {
    const size_t count = node.Size();
    EXPECT_GT(count, 0);
    EXPECT_TRUE(count == 1 || node.TotalBytes() <= node_max_bytes)
        << count << " elements in " << node.TotalBytes() << " bytes";
    ++nodes;
    elements += count;
    return true;
  });
  EXPECT_EQ(nodes, quicklist.NodeCount());
  EXPECT_EQ(elements, quicklist.Size());
}

std::vector<std::string> Values(const Quicklist& quicklist) {
  return quicklist.Empty() ? std::vector<std::string>{}
                           : quicklist.Range(0, quicklist.Size() - 1);
}

// With 64-byte nodes, four of these ten-character values fill a node.
constexpr size_t kSmallNodeMaxBytes = 64;
constexpr std::string_view kMatch = "match-0000";

// Pushes one value for each letter of `pattern`: kMatch for 'm' and a value
// found nowhere else for any other letter. Returns the values pushed.
std::vector<std::string> PushPattern(Quicklist* quicklist,
                                     std::string_view pattern) {
  std::vector<std::string> values;
  for (size_t index = 0; index < pattern.size(); ++index) {
    values.push_back(pattern[index] == 'm'
                         ? std::string(kMatch)
                         : "keep-" + std::to_string(10000 + index));
    EXPECT_TRUE(quicklist->RPush(values.back()));
  }
  return values;
}
}  // namespace

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

  ASSERT_EQ(quicklist.Remove(value + "2", 1, ListEnd::kTail), 1);
  EXPECT_EQ(quicklist.Range(0, 3),
            (std::vector<std::string>{value + "1", value + "2", value + "3",
                                      value + "4"}));

  ASSERT_EQ(quicklist.Remove(value + "2", 0, ListEnd::kHead), 1);
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

  quicklist.Trim(1, 3);
  EXPECT_EQ(quicklist.Range(0, 2),
            (std::vector<std::string>{value + "2", value + "3", value + "4"}));

  quicklist.Trim(1, 0);
  EXPECT_TRUE(quicklist.Empty());
  EXPECT_EQ(quicklist.NodeCount(), 0);
}

TEST(QuicklistTest, RemoveFromTailWithinOneNode) {
  Quicklist quicklist(256);
  for (std::string_view value : {"x", "keep", "x", "x", "keep", "x"}) {
    ASSERT_TRUE(quicklist.RPush(value));
  }
  ASSERT_EQ(quicklist.NodeCount(), 1);

  EXPECT_EQ(quicklist.Remove("x", 2, ListEnd::kTail), 2);
  EXPECT_EQ(quicklist.Range(0, quicklist.Size() - 1),
            (std::vector<std::string>{"x", "keep", "x", "keep"}));
}

TEST(QuicklistTest, ExtractsSingleListpackWithoutRebuilding) {
  Quicklist quicklist(256);
  ASSERT_TRUE(quicklist.RPush("one"));
  ASSERT_TRUE(quicklist.RPush("two"));
  ASSERT_EQ(quicklist.NodeCount(), 1);

  const Listpack listpack = quicklist.ExtractListpack();

  EXPECT_TRUE(quicklist.Empty());
  EXPECT_EQ(quicklist.NodeCount(), 0);
  ASSERT_EQ(listpack.Size(), 2);
  const std::optional<size_t> first = listpack.First();
  const std::optional<size_t> last = listpack.Last();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(listpack.Get(first.value_or(0)), "one");
  EXPECT_EQ(listpack.Get(last.value_or(0)), "two");
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
    quicklist.Trim(1, 0);
    EXPECT_EQ(quicklist.NodeCount(), 0);
    for (size_t index = 0; index < kNodes; ++index) {
      ASSERT_TRUE(quicklist.RPush("element"));
    }
  }
}

TEST(QuicklistTest, RemoveFromHeadMergesTheNodesItShrank) {
  Quicklist quicklist(kSmallNodeMaxBytes);
  std::vector<std::string> model = PushPattern(&quicklist,
                                               "kkkk"
                                               "kmmk"
                                               "kmmk"
                                               "kkkk"
                                               "kkkk");
  ASSERT_EQ(quicklist.NodeCount(), 5);

  EXPECT_EQ(quicklist.Remove(kMatch, 0, ListEnd::kHead), 4);

  // The second and third nodes kept two elements each and are now one.
  EXPECT_EQ(quicklist.NodeCount(), 4);
  EXPECT_EQ(fuzz::RemoveMatchesFromModel(&model, kMatch, 0, false), 4);
  EXPECT_EQ(Values(quicklist), model);
  ExpectValidNodes(quicklist, kSmallNodeMaxBytes);
  EXPECT_FALSE(HasNeighborsThatFitTogether(quicklist, kSmallNodeMaxBytes));
}

TEST(QuicklistTest, RemoveFromTailMergesTheNodesItShrank) {
  Quicklist quicklist(kSmallNodeMaxBytes);
  std::vector<std::string> model = PushPattern(&quicklist,
                                               "kmkk"
                                               "kkmm"
                                               "mmkk"
                                               "kkkk");
  ASSERT_EQ(quicklist.NodeCount(), 4);

  // The limit is reached in the second node, so the match in the first
  // stays.
  EXPECT_EQ(quicklist.Remove(kMatch, 4, ListEnd::kTail), 4);

  EXPECT_EQ(quicklist.NodeCount(), 3);
  EXPECT_EQ(fuzz::RemoveMatchesFromModel(&model, kMatch, 4, true), 4);
  EXPECT_EQ(model[1], kMatch);
  EXPECT_EQ(Values(quicklist), model);
  ExpectValidNodes(quicklist, kSmallNodeMaxBytes);
  EXPECT_FALSE(HasNeighborsThatFitTogether(quicklist, kSmallNodeMaxBytes));
}

TEST(QuicklistTest, RemoveMergesTheNeighborsOfANodeItEmptied) {
  // A value this long shares a node with nothing else here.
  const std::string alone(50, 'M');
  const std::vector<std::string> expected = {"keep-10000", "keep-10001",
                                             "keep-10002", "keep-10003"};
  const std::vector<std::pair<size_t, ListEnd>> cases = {
      {1, ListEnd::kHead}, {1, ListEnd::kTail}, {0, ListEnd::kHead}};
  for (const auto& [limit, from] : cases) {
    Quicklist quicklist(kSmallNodeMaxBytes);
    ASSERT_TRUE(quicklist.RPush(expected[0]));
    ASSERT_TRUE(quicklist.RPush(expected[1]));
    ASSERT_TRUE(quicklist.RPush(alone));
    ASSERT_TRUE(quicklist.RPush(expected[2]));
    ASSERT_TRUE(quicklist.RPush(expected[3]));
    ASSERT_EQ(quicklist.NodeCount(), 3);

    EXPECT_EQ(quicklist.Remove(alone, limit, from), 1) << limit;

    EXPECT_EQ(quicklist.NodeCount(), 1) << limit;
    EXPECT_EQ(Values(quicklist), expected) << limit;
    ExpectValidNodes(quicklist, kSmallNodeMaxBytes);
  }
}

TEST(QuicklistTest, RemoveLeavesNodesItDidNotChangeAsTheyWere) {
  Quicklist quicklist(kSmallNodeMaxBytes);
  const std::vector<std::string> values = PushPattern(&quicklist,
                                                      "kkkk"
                                                      "kkkk"
                                                      "kkkk");
  const std::vector<size_t> bytes = NodeBytes(quicklist);
  ASSERT_EQ(bytes.size(), 3);

  EXPECT_EQ(quicklist.Remove(kMatch, 0, ListEnd::kHead), 0);
  EXPECT_EQ(quicklist.Remove(kMatch, 2, ListEnd::kTail), 0);

  EXPECT_EQ(NodeBytes(quicklist), bytes);
  EXPECT_EQ(Values(quicklist), values);
}

TEST(QuicklistTest, TrimMergesTheNodeItShrankAtEitherEnd) {
  // Pushed at one end, these make nodes of 4, 1, 1 and 4 elements: the
  // fifth finds the first node full, and the long one shares a node with
  // nothing else here.
  std::vector<std::string> pushed;
  pushed.reserve(10);
  for (int index = 0; index < 9; ++index) {
    pushed.push_back("keep-" + std::to_string(10000 + index));
  }
  pushed.insert(pushed.begin() + 5, std::string(50, 'M'));

  Quicklist head_trimmed(kSmallNodeMaxBytes);
  Quicklist tail_trimmed(kSmallNodeMaxBytes);
  for (const std::string& value : pushed) {
    ASSERT_TRUE(head_trimmed.RPush(value));
    ASSERT_TRUE(tail_trimmed.LPush(value));
  }
  ASSERT_EQ(head_trimmed.NodeCount(), 4);
  ASSERT_EQ(tail_trimmed.NodeCount(), 4);

  // The two elements left in the node of four fit in the node next to it.
  head_trimmed.Trim(2, 9);
  std::vector<std::string> head_model = pushed;
  fuzz::TrimModel(&head_model, 2, 9);
  EXPECT_EQ(head_trimmed.NodeCount(), 3);
  EXPECT_EQ(Values(head_trimmed), head_model);
  ExpectValidNodes(head_trimmed, kSmallNodeMaxBytes);
  EXPECT_FALSE(HasNeighborsThatFitTogether(head_trimmed, kSmallNodeMaxBytes));

  tail_trimmed.Trim(0, 7);
  std::vector<std::string> tail_model(pushed.rbegin(), pushed.rend());
  fuzz::TrimModel(&tail_model, 0, 7);
  EXPECT_EQ(tail_trimmed.NodeCount(), 3);
  EXPECT_EQ(Values(tail_trimmed), tail_model);
  ExpectValidNodes(tail_trimmed, kSmallNodeMaxBytes);
  EXPECT_FALSE(HasNeighborsThatFitTogether(tail_trimmed, kSmallNodeMaxBytes));
}

TEST(QuicklistTest, AdoptListpackSplitsAnOversizedListpack) {
  Listpack listpack;
  std::vector<std::string> expected;
  for (int index = 0; index < 40; ++index) {
    expected.push_back("value:" + std::to_string(index));
    ASSERT_TRUE(listpack.Append(expected.back()));
  }

  Quicklist quicklist(kSmallNodeMaxBytes);
  quicklist.AdoptListpack(std::move(listpack));

  EXPECT_GT(quicklist.NodeCount(), 1);
  EXPECT_EQ(quicklist.Size(), expected.size());
  ExpectValidNodes(quicklist, kSmallNodeMaxBytes);
  EXPECT_EQ(Values(quicklist), expected);
  // Each element is found by its index, from either end.
  for (size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(quicklist.Range(index, index),
              std::vector<std::string>{expected[index]})
        << index;
  }
}

TEST(QuicklistTest, RandomOperationsMatchAModel) {
  // Integers and strings, one of them longer than a node may be, so that
  // nodes hold from one element to dozens.
  const std::vector<std::string> values = {"x",
                                           "",
                                           "7",
                                           "-129",
                                           "12345678901",
                                           std::string(20, 'm'),
                                           std::string(90, 'l')};
  // A fixed linear congruential sequence keeps the cases reproducible.
  uint32_t state = 11;
  const auto random = [&state] {
    state = (state * 1664525U) + 1013904223U;
    return state >> 8;
  };
  // LSET may split a node into pieces that fit together with a neighbor, so
  // the check that no neighbors fit together runs only without it.
  for (const bool with_set : {false, true}) {
    Quicklist quicklist(kSmallNodeMaxBytes);
    std::vector<std::string> model;
    for (int step = 0; step < 3000; ++step) {
      const std::string& value = values[random() % values.size()];
      const uint32_t operation = random() % 12;
      const bool from_tail = random() % 2 == 0;
      const ListEnd end = from_tail ? ListEnd::kTail : ListEnd::kHead;
      if (operation == 7 || operation == 8) {
        std::optional<std::string> popped;
        const bool found = quicklist.Pop(
            end,
            [&popped](std::string_view element) { popped.emplace(element); });
        ASSERT_EQ(found, !model.empty()) << step;
        if (found) {
          ASSERT_EQ(popped, from_tail ? model.back() : model.front()) << step;
          model.erase(from_tail ? model.end() - 1 : model.begin());
        }
      } else if (operation == 9) {
        const size_t limit = random() % 4;
        ASSERT_EQ(quicklist.Remove(value, limit, end),
                  fuzz::RemoveMatchesFromModel(&model, value, limit, from_tail))
            << step;
      } else if (operation == 10) {
        // Mostly a few elements off each end, as a capped list is trimmed,
        // and now and then any range at all.
        const bool any_range = random() % 16 == 0;
        const size_t start =
            any_range ? random() % (model.size() + 2) : random() % 4;
        const size_t stop =
            any_range
                ? random() % (model.size() + 2)
                : model.size() - std::min<size_t>(model.size(), random() % 4);
        quicklist.Trim(start, stop);
        fuzz::TrimModel(&model, start, stop);
      } else if (operation == 11 && with_set && !model.empty()) {
        const size_t index = random() % model.size();
        ASSERT_TRUE(quicklist.Set(index, value)) << step;
        model[index] = value;
      } else if (operation < 5 || operation == 11) {
        ASSERT_TRUE(quicklist.RPush(value));
        model.push_back(value);
      } else {
        ASSERT_TRUE(quicklist.LPush(value));
        model.insert(model.begin(), value);
      }

      ASSERT_EQ(Values(quicklist), model) << step;
      ExpectValidNodes(quicklist, kSmallNodeMaxBytes);
      if (!with_set) {
        ASSERT_FALSE(HasNeighborsThatFitTogether(quicklist, kSmallNodeMaxBytes))
            << step;
      }
      if (model.empty()) {
        continue;
      }
      // A seek from the tail, and one to any index, use the element count
      // each node keeps.
      std::vector<std::string> reversed;
      ASSERT_TRUE(quicklist.ForEachReverse(
          0, model.size() - 1, [&reversed](std::string_view element) {
            reversed.emplace_back(element);
            return true;
          }));
      ASSERT_EQ(reversed,
                std::vector<std::string>(model.rbegin(), model.rend()))
          << step;
      const size_t index = random() % model.size();
      ASSERT_EQ(quicklist.Range(index, index),
                std::vector<std::string>{model[index]})
          << step;
    }
  }
}
}  // namespace redis_simple::containers
