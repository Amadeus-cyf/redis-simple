#include "containers/compact_string.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include "utils/check.h"

namespace redis_simple::containers {
void CompactString::Assign(std::string_view value) {
  RS_CHECK(value.size() <= std::numeric_limits<uint32_t>::max());
  if (value.size() <= kInlineCapacity) {
    // Copied out first: `value` may view the heap block freed below.
    std::array<char, kInlineCapacity> bytes{};
    if (!value.empty()) {
      std::memcpy(bytes.data(), value.data(), value.size());
    }
    FreeHeap();
    if (!value.empty()) {
      std::memcpy(storage_.data(), bytes.data(), value.size());
    }
    SetInlineSize(value.size());
    return;
  }
  if (!IsInline() && HeapSize() == value.size()) {
    // Same length: reuse the block. memmove, since `value` may overlap it.
    std::memmove(HeapData(), value.data(), value.size());
    return;
  }
  // A new block is filled before the old one is freed, for the same reason.
  char* const data = new char[value.size()];
  std::memcpy(data, value.data(), value.size());
  FreeHeap();
  SetHeap(data, static_cast<uint32_t>(value.size()));
}
}  // namespace redis_simple::containers
