#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data_types/list/list.h"
#include "fuzz/fuzz_input.h"
#include "fuzz/sequence_model.h"

namespace redis_simple::fuzz {
namespace {
using list::List;

void Verify(const List& list, const std::vector<std::string>& model) {
  Require(list.Size() == model.size());
  const size_t stop = model.empty() ? 0 : model.size() - 1;
  Require(list.Range(0, stop) == model);

  std::vector<std::string> visited;
  Require(list.ForEach(0, stop, [&visited](std::string_view value) {
    visited.emplace_back(value);
    return true;
  }));
  Require(visited == model);
}

void RunOperations(FuzzInput* input) {
  constexpr size_t kListpackMaxBytes = 64;
  auto list = List::Create(kListpackMaxBytes);
  std::vector<std::string> model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 8;
    const std::string value = input->ReadValue(128);
    const size_t index = input->ReadIndex(model.size() + 2);

    switch (operation) {
      case 0:
        Require(list->LPush(value));
        model.insert(model.begin(), value);
        break;
      case 1:
        Require(list->RPush(value));
        model.push_back(value);
        break;
      case 2: {
        const auto actual = list->LPop();
        const std::optional<std::string> expected =
            model.empty() ? std::nullopt
                          : std::optional<std::string>(model.front());
        Require(actual == expected);
        if (!model.empty()) {
          model.erase(model.begin());
        }
        break;
      }
      case 3: {
        const auto actual = list->RPop();
        const std::optional<std::string> expected =
            model.empty() ? std::nullopt
                          : std::optional<std::string>(model.back());
        Require(actual == expected);
        if (!model.empty()) {
          model.pop_back();
        }
        break;
      }
      case 4: {
        const bool expected = index < model.size();
        Require(list->Set(index, value) == expected);
        if (expected) {
          model[index] = value;
        }
        break;
      }
      case 5: {
        const size_t limit = input->ReadIndex(6);
        const auto direction = (input->ReadByte() & 1U) == 0
                                   ? List::RemoveDirection::kFromHead
                                   : List::RemoveDirection::kFromTail;
        const bool from_tail = direction == List::RemoveDirection::kFromTail;
        const size_t expected =
            RemoveMatchesFromModel(&model, value, limit, from_tail);
        Require(ValueOrAbort(list->Remove(value, limit, direction)) ==
                expected);
        break;
      }
      case 6: {
        const size_t stop = input->ReadIndex(model.size() + 2);
        Require(list->Trim(index, stop));
        TrimModel(&model, index, stop);
        break;
      }
      case 7:
        Require(list->At(index) ==
                (index < model.size() ? std::optional<std::string>(model[index])
                                      : std::nullopt));
        break;
      default:
        break;
    }
    Verify(*list, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
