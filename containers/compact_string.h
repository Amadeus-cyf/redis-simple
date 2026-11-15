#ifndef REDIS_SIMPLE_CONTAINERS_COMPACT_STRING_H_
#define REDIS_SIMPLE_CONTAINERS_COMPACT_STRING_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#include "utils/check.h"

namespace redis_simple::containers {
// A byte string in 16 bytes, half a std::string: up to kInlineCapacity bytes
// are stored in the object, and longer ones in a single heap block of exactly
// their length. It holds values that are written whole and read as views,
// such as hash field values, so it has no capacity to grow into.
class CompactString {
 public:
  static constexpr size_t kInlineCapacity = 15;

  CompactString() { SetInlineSize(0); }
  explicit CompactString(std::string_view value) : CompactString() {
    Assign(value);
  }
  CompactString(const CompactString& other) : CompactString() {
    Assign(other.View());
  }
  CompactString& operator=(const CompactString& other) {
    Assign(other.View());
    return *this;
  }
  CompactString(CompactString&& other) noexcept : storage_(other.storage_) {
    other.SetInlineSize(0);
  }
  CompactString& operator=(CompactString&& other) noexcept {
    if (this != &other) {
      FreeHeap();
      storage_ = other.storage_;
      other.SetInlineSize(0);
    }
    return *this;
  }
  ~CompactString() { FreeHeap(); }

  std::string_view View() const {
    if (IsInline()) {
      return {storage_.data(), InlineSize()};
    }
    return {HeapData(), HeapSize()};
  }
  size_t Size() const { return IsInline() ? InlineSize() : HeapSize(); }

  // Replaces the contents with `value`, which may view this string's own
  // bytes.
  void Assign(std::string_view value) {
    RS_CHECK(value.size() <= std::numeric_limits<uint32_t>::max());
    if (value.size() <= kInlineCapacity) {
      // Copied out first: `value` may view the heap block freed below.
      std::array<char, kInlineCapacity> bytes;
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

 private:
  // The last byte tells the two forms apart: an inline string's length
  // (0 to kInlineCapacity), or kHeapTag. A heap string keeps its pointer in
  // bytes 0 to 7 and its length in bytes 8 to 11.
  static constexpr size_t kTagOffset = 15;
  static constexpr unsigned char kHeapTag = 0xff;
  static constexpr size_t kSizeOffset = sizeof(char*);
  static_assert(kSizeOffset + sizeof(uint32_t) <= kTagOffset);

  bool IsInline() const {
    return static_cast<unsigned char>(storage_[kTagOffset]) != kHeapTag;
  }
  size_t InlineSize() const {
    return static_cast<unsigned char>(storage_[kTagOffset]);
  }
  void SetInlineSize(size_t size) {
    storage_[kTagOffset] = static_cast<char>(size);
  }
  char* HeapData() const {
    char* data = nullptr;
    std::memcpy(&data, storage_.data(), sizeof(data));
    return data;
  }
  size_t HeapSize() const {
    uint32_t size = 0;
    std::memcpy(&size, storage_.data() + kSizeOffset, sizeof(size));
    return size;
  }
  void SetHeap(char* data, uint32_t size) {
    std::memcpy(storage_.data(), &data, sizeof(data));
    std::memcpy(storage_.data() + kSizeOffset, &size, sizeof(size));
    storage_[kTagOffset] = static_cast<char>(kHeapTag);
  }
  void FreeHeap() {
    if (!IsInline()) {
      delete[] HeapData();
      SetInlineSize(0);
    }
  }

  alignas(8) std::array<char, 16> storage_;
};
static_assert(sizeof(CompactString) == 16);
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_COMPACT_STRING_H_
