#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "containers/skiplist.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
using IntSkiplist = containers::Skiplist<int64_t>;

std::vector<int64_t> ToVector(const std::set<int64_t>& model) {
  return {model.begin(), model.end()};
}

void Verify(const IntSkiplist& skiplist, const std::set<int64_t>& model) {
  const auto expected = ToVector(model);
  Require(skiplist.Size() == expected.size());

  std::vector<int64_t> visited;
  for (auto it = skiplist.Begin(); it.Valid(); it.Next()) {
    visited.push_back(*it);
  }
  Require(visited == expected);

  // Backward links, which every insert, delete, and update rewrites.
  std::vector<int64_t> visited_backward;
  for (auto it = skiplist.Last(); it.Valid(); --it) {
    visited_backward.push_back(*it);
  }
  std::ranges::reverse(visited_backward);
  Require(visited_backward == expected);

  for (size_t rank = 0; rank < expected.size(); ++rank) {
    Require(skiplist.FindKeyByRank(static_cast<int64_t>(rank)) ==
            expected[rank]);
    Require(skiplist.FindRankOfKey(expected[rank]) == rank);
  }
  if (expected.empty()) {
    return;
  }

  IntSkiplist::RankRange ranks{.min = 0,
                               .max = expected.size() - 1,
                               .min_exclusive = false,
                               .max_exclusive = false,
                               .limit = std::nullopt,
                               .reverse = false};
  Require(skiplist.RangeByRank(ranks) == expected);
  auto reversed = expected;
  std::ranges::reverse(reversed);
  ranks.reverse = true;
  Require(skiplist.RangeByRank(ranks) == reversed);

  IntSkiplist::KeyRange<int64_t> keys{.min = expected.front(),
                                      .max = expected.back(),
                                      .min_exclusive = false,
                                      .max_exclusive = false,
                                      .limit = std::nullopt,
                                      .reverse = false};
  Require(skiplist.RangeByKey(keys) == expected);
  Require(skiplist.Count(keys) == expected.size());
  keys.reverse = true;
  Require(skiplist.RangeByKey(keys) == reversed);

  // Exclusive bounds drop exactly the endpoint keys.
  keys = {.min = expected.front(),
          .max = expected.back(),
          .min_exclusive = true,
          .max_exclusive = true,
          .limit = std::nullopt,
          .reverse = false};
  const size_t interior = expected.size() > 2 ? expected.size() - 2 : 0;
  Require(skiplist.Count(keys) == interior);
  Require(skiplist.RangeByKey(keys).size() == interior);
}

// What a limit selects from `keys`, a range's keys in the order visited.
std::vector<int64_t> Limited(const std::vector<int64_t>& keys, size_t offset,
                             size_t count) {
  if (offset >= keys.size()) {
    return {};
  }
  const auto first = keys.begin() + static_cast<std::ptrdiff_t>(offset);
  return {first, first + static_cast<std::ptrdiff_t>(
                             std::min(count, keys.size() - offset))};
}

// Checks the key range and the rank range that cover every key, forward and
// reversed, with a limit. An offset of 10 or more is reached by rank, a
// smaller one by stepping, so `offset` should take values on both sides.
void VerifyLimit(const IntSkiplist& skiplist, const std::set<int64_t>& model,
                 size_t offset, size_t count) {
  if (model.empty()) {
    return;
  }
  const std::vector<int64_t> keys(model.begin(), model.end());
  const std::vector<int64_t> reversed(model.rbegin(), model.rend());
  const IntSkiplist::Limit limit{.offset = offset, .count = count};

  IntSkiplist::KeyRange<int64_t> by_key{.min = keys.front(),
                                        .max = keys.back(),
                                        .min_exclusive = false,
                                        .max_exclusive = false,
                                        .limit = limit,
                                        .reverse = false};
  Require(skiplist.RangeByKey(by_key) == Limited(keys, offset, count));
  by_key.reverse = true;
  Require(skiplist.RangeByKey(by_key) == Limited(reversed, offset, count));

  IntSkiplist::RankRange by_rank{.min = 0,
                                 .max = keys.size() - 1,
                                 .min_exclusive = false,
                                 .max_exclusive = false,
                                 .limit = limit,
                                 .reverse = false};
  Require(skiplist.RangeByRank(by_rank) == Limited(keys, offset, count));
  by_rank.reverse = true;
  Require(skiplist.RangeByRank(by_rank) == Limited(reversed, offset, count));
}

void RunOperations(FuzzInput* input) {
  IntSkiplist skiplist;
  std::set<int64_t> model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 5;
    const int64_t value = input->ReadInt64();
    switch (operation) {
      case 0:
        Require(skiplist.Insert(value) == value);
        model.insert(value);
        break;
      case 1:
        Require(skiplist.Delete(value) == (model.erase(value) != 0));
        break;
      case 2:
        Require(skiplist.Contains(value) == model.contains(value));
        break;
      case 3: {
        const int64_t replacement = input->ReadInt64();
        const bool exists = model.contains(value);
        const bool replacement_available =
            replacement == value || !model.contains(replacement);
        if (replacement_available) {
          Require(skiplist.Update(value, [replacement](int64_t& key) {
            key = replacement;
          }) == exists);
          if (exists) {
            model.erase(value);
            model.insert(replacement);
          }
        }
        break;
      }
      case 4:
        skiplist.Clear();
        model.clear();
        break;
      default:
        break;
    }
    Verify(skiplist, model);
    // The operation's value picks the limit: offsets up to 31, counts up to 7.
    const auto bits = static_cast<uint64_t>(value);
    VerifyLimit(skiplist, model, bits % 32, (bits >> 5U) % 8);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
