#include <sys/uio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fuzz/fuzz_input.h"
#include "memory/reply_buffer.h"

namespace redis_simple::fuzz {
namespace {
void Verify(memory::ReplyBuffer* buffer, const std::string& model) {
  Require(buffer->PendingBytes() == model.size());
  Require(buffer->Empty() == model.empty());

  std::vector<iovec> blocks;
  buffer->FillBlocks(&blocks);
  std::string actual;
  actual.reserve(model.size());
  for (const auto& block : blocks) {
    Require(block.iov_base != nullptr);
    Require(block.iov_len > 0);
    actual.append(static_cast<const char*>(block.iov_base), block.iov_len);
  }
  Require(actual == model);
}

void BoundPendingBytes(memory::ReplyBuffer* buffer, std::string* model) {
  constexpr size_t kMaxPendingBytes = 32768;
  constexpr size_t kRetainedBytes = 16384;
  if (model->size() <= kMaxPendingBytes) {
    return;
  }
  const size_t consumed = model->size() - kRetainedBytes;
  buffer->Consume(consumed);
  model->erase(0, consumed);
}

void RunOperations(FuzzInput* input) {
  memory::ReplyBuffer buffer;
  std::string model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 4;
    std::string value = input->ReadValue(256);
    switch (operation) {
      case 0:
        Require(buffer.Append(value.data(), value.size()) == value.size());
        model.append(value);
        break;
      case 1: {
        const size_t value_size = value.size();
        model.append(value);
        Require(buffer.Append(std::move(value)) == value_size);
        break;
      }
      case 2: {
        const size_t consumed = input->ReadIndex(model.size() + 4097);
        buffer.Consume(consumed);
        model.erase(0, std::min(consumed, model.size()));
        break;
      }
      case 3:
        value = PadToSize(std::move(value), 5000);
        Require(buffer.Append(value.data(), value.size()) == value.size());
        model.append(value);
        break;
      default:
        break;
    }
    BoundPendingBytes(&buffer, &model);
    Verify(&buffer, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
