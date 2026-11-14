#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "containers/listpack.h"
#include "fuzz/fuzz_input.h"
#include "fuzz/sequence_model.h"

namespace redis_simple::fuzz {
namespace {
using containers::Listpack;

void Verify(const Listpack& listpack, const std::vector<std::string>& model) {
  Require(listpack.Size() == model.size());
  Require(listpack.TotalBytes() >= Listpack::kListpackHeaderSize + 1);
  Require(listpack.First().has_value() == !model.empty());
  Require(listpack.Last().has_value() == !model.empty());

  std::vector<std::string> forward;
  const size_t stop = model.empty() ? 0 : model.size() - 1;
  Require(listpack.ForEach(0, stop, [&forward](std::string_view value) {
    forward.emplace_back(value);
    return true;
  }));
  Require(forward == model);

  std::vector<std::string> reverse;
  Require(listpack.ForEachReverse(0, stop, [&reverse](std::string_view value) {
    reverse.emplace_back(value);
    return true;
  }));
  const std::vector<std::string> expected_reverse(model.rbegin(), model.rend());
  Require(reverse == expected_reverse);

  for (size_t index = 0; index < model.size(); ++index) {
    const auto listpack_index = listpack.OffsetAt(index);
    Require(listpack.Get(ValueOrAbort(listpack_index)) == model[index]);
  }
  Require(!listpack.OffsetAt(model.size()).has_value());
}

void RunOperations(FuzzInput* input) {
  Listpack listpack;
  std::vector<std::string> model;
  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 13;
    const std::string value = input->ReadValue(96);
    const size_t index = input->ReadIndex(model.size() + 2);
    const size_t limit = input->ReadIndex(6);

    switch (operation) {
      case 0:
        Require(listpack.Append(value));
        model.push_back(value);
        break;
      case 1:
        Require(listpack.Prepend(value));
        model.insert(model.begin(), value);
        break;
      case 2: {
        const int64_t integer = input->ReadInt64();
        Require(listpack.Append(integer));
        model.push_back(std::to_string(integer));
        break;
      }
      case 3: {
        const int64_t integer = input->ReadInt64();
        Require(listpack.Prepend(integer));
        model.insert(model.begin(), std::to_string(integer));
        break;
      }
      case 4:
        if (index < model.size()) {
          const auto listpack_index = listpack.OffsetAt(index);
          Require(listpack.Insert(ValueOrAbort(listpack_index), value));
          model.insert(model.begin() + static_cast<std::ptrdiff_t>(index),
                       value);
        }
        break;
      case 5:
        if (index < model.size()) {
          const auto listpack_index = listpack.OffsetAt(index);
          Require(listpack.Replace(ValueOrAbort(listpack_index), value));
          model[index] = value;
        }
        break;
      case 6:
        if (index < model.size()) {
          const auto listpack_index = listpack.OffsetAt(index);
          listpack.Delete(ValueOrAbort(listpack_index));
          model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
        }
        break;
      case 7:
        if (index < model.size()) {
          const auto listpack_index = listpack.OffsetAt(index);
          const size_t expected = std::min(limit, model.size() - index);
          Require(listpack.DeleteRange(ValueOrAbort(listpack_index), limit) ==
                  expected);
          model.erase(
              model.begin() + static_cast<std::ptrdiff_t>(index),
              model.begin() + static_cast<std::ptrdiff_t>(index + expected));
        }
        break;
      case 8: {
        const bool from_tail = (input->ReadByte() & 1U) != 0;
        const size_t expected =
            RemoveMatchesFromModel(&model, value, limit, from_tail);
        Require(listpack.DeleteMatching(value, limit, from_tail) == expected);
        break;
      }
      case 9: {
        const std::string second = input->ReadValue(96);
        const std::vector<Listpack::ListpackEntry> entries = {
            {value, 0, false}, {second, 0, false}};
        Require(listpack.BatchAppend(entries));
        model.push_back(value);
        model.push_back(second);
        break;
      }
      case 10: {
        const std::string second = input->ReadValue(96);
        const std::vector<Listpack::ListpackEntry> entries = {
            {value, 0, false}, {second, 0, false}};
        Require(listpack.BatchPrepend(entries));
        model.insert(model.begin(), {value, second});
        break;
      }
      case 11:
        if (index < model.size()) {
          const std::string second = input->ReadValue(96);
          const std::vector<Listpack::ListpackEntry> entries = {
              {value, 0, false}, {second, 0, false}};
          const auto listpack_index = listpack.OffsetAt(index);
          Require(listpack.BatchInsert(ValueOrAbort(listpack_index), entries));
          model.insert(model.begin() + static_cast<std::ptrdiff_t>(index),
                       {value, second});
        }
        break;
      case 12: {
        const auto found = listpack.Find(value);
        const bool expected =
            std::find(model.begin(), model.end(), value) != model.end();
        Require(found.has_value() == expected);
        if (found.has_value()) {
          Require(listpack.Get(found.value()) == value);
        }
        break;
      }
      default:
        break;
    }
    Verify(listpack, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
