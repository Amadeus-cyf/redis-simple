#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "fuzz/fuzz_input.h"
#include "memory/dynamic_buffer.h"

namespace redis_simple::fuzz {
namespace {
std::optional<std::string> ReadModelLine(std::string* model) {
  const size_t newline = model->find('\n');
  if (newline == std::string::npos) {
    return std::nullopt;
  }
  size_t line_size = newline;
  if (line_size > 0 && (*model)[line_size - 1] == '\r') {
    --line_size;
  }
  std::string line = model->substr(0, line_size);
  model->erase(0, newline + 1);
  return line;
}

void Verify(const memory::DynamicBuffer& buffer, const std::string& model) {
  Require(buffer.View() == std::string_view(model));
  Require(buffer.ToString() == model);
  Require(buffer.Empty() == model.empty());
  Require(buffer.Consumed() <= buffer.Size());
  Require(buffer.Size() <= buffer.Capacity());
}

void RunOperations(FuzzInput* input) {
  memory::DynamicBuffer buffer;
  std::string model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 7;
    std::string value = input->ReadValue(256);
    switch (operation) {
      case 0:
        buffer.Append(value.data(), value.size());
        model.append(value);
        break;
      case 1: {
        const size_t consumed = input->ReadIndex(model.size() + 16);
        buffer.Consume(consumed);
        model.erase(0, std::min(consumed, model.size()));
        break;
      }
      case 2:
        buffer.Compact();
        Require(buffer.Size() == model.size());
        Require(buffer.Consumed() == 0);
        break;
      case 3: {
        const auto expected = ReadModelLine(&model);
        const auto actual = buffer.ReadLineView();
        Require(actual.has_value() == expected.has_value());
        if (actual.has_value()) {
          Require(ValueOrAbort(actual) == ValueOrAbort(expected));
        }
        break;
      }
      case 4: {
        const auto expected = ReadModelLine(&model);
        const std::string actual = buffer.ReadLine();
        Require(actual == expected.value_or(""));
        break;
      }
      case 5:
        buffer.Clear();
        model.clear();
        break;
      case 6:
        value = PadToSize(std::move(value), 5000);
        buffer.Append(value.data(), value.size());
        model.append(value);
        break;
      default:
        break;
    }
    Verify(buffer, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
