#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "containers/dict.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
using StringDict = containers::Dict<std::string, std::string>;
using DictModel = std::unordered_map<std::string, std::string>;

DictModel ReadWithIterator(const StringDict& dict) {
  DictModel values;
  auto it = StringDict::Iterator(&dict);
  it.SeekToFirst();
  while (it.Valid()) {
    Require(values.emplace(it.Key(), it.Value()).second);
    it.Next();
  }
  return values;
}

DictModel ReadWithScan(StringDict* dict) {
  DictModel values;
  std::optional<size_t> cursor = 0;
  while (cursor.has_value()) {
    cursor = dict->Scan(
        *cursor, [&values](std::string_view key, const std::string& value) {
          Require(values.emplace(key, value).second);
        });
  }
  return values;
}

void Verify(StringDict* dict, const DictModel& model) {
  Require(dict->Size() == model.size());
  for (const auto& [key, value] : model) {
    const auto* actual = dict->FindValue(std::string_view(key));
    Require(actual != nullptr && *actual == value);
  }
  Require(ReadWithIterator(*dict) == model);
}

// Restores the default resize policy when one input's operations end, so
// the next input starts from the same state.
class ResizePolicyReset {
 public:
  ResizePolicyReset() = default;
  ResizePolicyReset(const ResizePolicyReset&) = delete;
  ResizePolicyReset& operator=(const ResizePolicyReset&) = delete;
  ResizePolicyReset(ResizePolicyReset&&) = delete;
  ResizePolicyReset& operator=(ResizePolicyReset&&) = delete;
  ~ResizePolicyReset() {
    containers::SetDictResizePolicy(containers::DictResizePolicy::kEnabled);
  }
};

containers::DictResizePolicy PolicyFor(uint8_t selector) {
  switch (selector % 3) {
    case 0:
      return containers::DictResizePolicy::kEnabled;
    case 1:
      return containers::DictResizePolicy::kAvoid;
    default:
      return containers::DictResizePolicy::kForbidden;
  }
}

void RunOperations(FuzzInput* input) {
  const ResizePolicyReset reset;
  auto dict = StringDict::Create(input->ReadIndex(8) + 1);
  Require(dict != nullptr);
  DictModel model;

  for (size_t operation_count = 0; operation_count < 256 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 8;
    const std::string key = input->ReadValue(64);
    const std::string value = input->ReadValue(96);
    switch (operation) {
      case 0:
        dict->Set(key, value);
        model[key] = value;
        break;
      case 1:
        Require(dict->Insert(key, value) == model.emplace(key, value).second);
        break;
      case 2:
        Require(dict->Delete(std::string_view(key)) == (model.erase(key) != 0));
        break;
      case 3: {
        const auto* actual = dict->FindValue(std::string_view(key));
        const auto expected = model.find(key);
        Require((actual == nullptr) == (expected == model.end()));
        if (actual != nullptr) {
          Require(*actual == expected->second);
        }
        break;
      }
      case 4:
        Require(ReadWithScan(dict.get()) == model);
        break;
      case 5:
        if (value.size() % 2 == 0) {
          dict->Clear();
        } else {
          // In steps, as a flushed keyspace is freed; the Dict is usable
          // again once the last step reports it empty.
          while (!dict->ClearSome(key.size() + 1)) {
          }
        }
        model.clear();
        break;
      case 6: {
        auto extracted = dict->Extract(std::string_view(key));
        const auto expected = model.find(key);
        Require(extracted.has_value() == (expected != model.end()));
        if (extracted.has_value()) {
          Require(*extracted == expected->second);
          model.erase(expected);
        }
        break;
      }
      case 7:
        // As a fork and the end of a child do in the server: tables stop
        // growing, shrinking, or rehashing, part-way through or not.
        containers::SetDictResizePolicy(
            PolicyFor(key.empty() ? 0 : static_cast<uint8_t>(key[0])));
        break;
      default:
        break;
    }
    Verify(dict.get(), model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
