#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "data_types/hash/hash.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
using HashModel = std::unordered_map<std::string, std::string>;

void Verify(const hash::Hash& hash, const HashModel& model) {
  Require(hash.Size() == model.size());
  for (const auto& [field, value] : model) {
    Require(hash.Exists(field));
    Require(hash.Get(field) == value);
  }

  HashModel visited;
  Require(hash.ForEachEntry(
      [&visited](std::string_view field, std::string_view value) {
        visited.emplace(field, value);
        return true;
      }));
  Require(visited == model);
}

void RunOperations(FuzzInput* input) {
  auto hash = hash::Hash::Create();
  HashModel model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 6;
    std::string field = input->ReadValue(96);
    std::string value = input->ReadValue(96);
    if (operation == 5) {
      field = PadToSize(std::move(field), 65);
    }

    switch (operation) {
      case 0:
      case 5: {
        const bool inserted = !model.contains(field);
        Require(hash->Set(field, value) == inserted);
        model[field] = value;
        break;
      }
      case 1:
        Require(hash->Delete(field) == (model.erase(field) != 0));
        break;
      case 2: {
        const auto it = model.find(field);
        Require(hash->Get(field) ==
                (it == model.end() ? std::optional<std::string>()
                                   : std::optional<std::string>(it->second)));
        break;
      }
      case 3:
        Require(hash->Exists(field) == model.contains(field));
        break;
      case 4: {
        const auto entries = hash->Entries();
        HashModel actual;
        for (const auto& entry : entries) {
          actual.emplace(entry.field, entry.value);
        }
        Require(actual == model);
        break;
      }
      default:
        break;
    }
    Verify(*hash, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
