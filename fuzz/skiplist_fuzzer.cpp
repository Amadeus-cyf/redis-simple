#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "fuzz/fuzz_input.h"
#include "memory/skiplist.h"

namespace redis_simple::fuzz {
namespace {
using IntSkiplist = in_memory::Skiplist<int64_t>;

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
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
