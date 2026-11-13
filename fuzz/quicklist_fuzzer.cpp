#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz/fuzz_input.h"
#include "fuzz/sequence_model.h"
#include "memory/quicklist.h"

namespace redis_simple::fuzz {
namespace {
using memory::Quicklist;

void Verify(const Quicklist& quicklist, const std::vector<std::string>& model) {
  Require(quicklist.Size() == model.size());
  Require(quicklist.Empty() == model.empty());
  Require(quicklist.NodeCount() <= model.size());
  Require((quicklist.NodeCount() == 0) == model.empty());

  const size_t stop = model.empty() ? 0 : model.size() - 1;
  Require(quicklist.Range(0, stop) == model);

  std::vector<std::string> forward;
  Require(quicklist.ForEach(0, stop, [&forward](std::string_view value) {
    forward.emplace_back(value);
    return true;
  }));
  Require(forward == model);

  std::vector<std::string> reverse;
  Require(quicklist.ForEachReverse(0, stop, [&reverse](std::string_view value) {
    reverse.emplace_back(value);
    return true;
  }));
  const std::vector<std::string> expected_reverse(model.rbegin(), model.rend());
  Require(reverse == expected_reverse);
  Require(quicklist.ListpackBytes().has_value() ==
          (quicklist.NodeCount() <= 1));
}

void RunOperations(FuzzInput* input) {
  const size_t node_max_bytes = input->ReadIndex(250) + 7;
  Quicklist quicklist(node_max_bytes);
  std::vector<std::string> model;
  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 8;
    const std::string value = input->ReadValue(128);
    const size_t index = input->ReadIndex(model.size() + 2);

    switch (operation) {
      case 0:
        Require(quicklist.LPush(value));
        model.insert(model.begin(), value);
        break;
      case 1:
        Require(quicklist.RPush(value));
        model.push_back(value);
        break;
      case 2: {
        const auto result = quicklist.LPop();
        const std::optional<std::string> expected =
            model.empty() ? std::nullopt
                          : std::optional<std::string>(model.front());
        Require(result == expected);
        if (!model.empty()) {
          model.erase(model.begin());
        }
        break;
      }
      case 3: {
        const auto result = quicklist.RPop();
        const std::optional<std::string> expected =
            model.empty() ? std::nullopt
                          : std::optional<std::string>(model.back());
        Require(result == expected);
        if (!model.empty()) {
          model.pop_back();
        }
        break;
      }
      case 4: {
        const bool expected = index < model.size();
        Require(quicklist.Set(index, value) == expected);
        if (expected) {
          model[index] = value;
        }
        break;
      }
      case 5: {
        const size_t limit = input->ReadIndex(6);
        const auto direction = (input->ReadByte() & 1U) == 0
                                   ? Quicklist::RemoveDirection::kFromHead
                                   : Quicklist::RemoveDirection::kFromTail;
        const bool from_tail =
            direction == Quicklist::RemoveDirection::kFromTail;
        const size_t expected =
            RemoveMatchesFromModel(&model, value, limit, from_tail);
        const auto removed = quicklist.Remove(value, limit, direction);
        Require(ValueOrAbort(removed) == expected);
        break;
      }
      case 6: {
        const size_t stop = input->ReadIndex(model.size() + 2);
        Require(quicklist.Trim(index, stop));
        TrimModel(&model, index, stop);
        break;
      }
      case 7: {
        const size_t stop = input->ReadIndex(model.size() + 2);
        std::vector<std::string> expected;
        if (index <= stop && index < model.size()) {
          const size_t bounded_stop = std::min(stop, model.size() - 1);
          expected.assign(
              model.begin() + static_cast<std::ptrdiff_t>(index),
              model.begin() + static_cast<std::ptrdiff_t>(bounded_stop + 1));
        }
        Require(quicklist.Range(index, stop) == expected);
        break;
      }
      default:
        break;
    }
    Verify(quicklist, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
