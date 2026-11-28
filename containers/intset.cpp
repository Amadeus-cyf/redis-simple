#include "containers/intset.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "utils/check.h"
#include "utils/size_class.h"

namespace redis_simple::containers {
namespace {
// Storage whose bytes are all written before they are read, so it is not
// zero-filled.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): compact byte storage
std::unique_ptr<unsigned char[]> AllocateContents(size_t bytes) {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): compact byte storage
  return std::make_unique_for_overwrite<unsigned char[]>(bytes);
}

// A Redis blob starts with the width of its values and their count, each a
// 32-bit integer.
constexpr size_t kBlobHeaderBytes = 2 * sizeof(uint32_t);

// The 32-bit integer `bytes` starts with.
uint32_t LoadLittleEndian32(std::string_view bytes) {
  uint32_t word = 0;
  std::memcpy(&word, bytes.data(), sizeof(word));
  if constexpr (std::endian::native == std::endian::big) {
    word = std::byteswap(word);
  }
  return word;
}

void AppendLittleEndian32(uint32_t word, std::string* const out) {
  if constexpr (std::endian::native == std::endian::big) {
    word = std::byteswap(word);
  }
  std::array<char, sizeof(word)> bytes{};
  std::memcpy(bytes.data(), &word, sizeof(word));
  out->append(bytes.data(), bytes.size());
}

// Converts `values`, integers of `width` bytes each, between the host's byte
// order and a blob's little-endian one. The two are the same on a
// little-endian host, where the values of a set and of its blob are the same
// bytes.
void SwapBetweenHostAndLittleEndian(std::span<char> values, size_t width) {
  if constexpr (std::endian::native == std::endian::big) {
    for (size_t offset = 0; offset < values.size(); offset += width) {
      std::ranges::reverse(values.subspan(offset, width));
    }
  }
}
}  // namespace

Intset::Intset() : Intset(0, kInt16) {}

Intset::Intset(uint32_t length, EncodingType encoding)
    : contents_(AllocateContents(CapacityBytes(length, encoding))),
      length_(length),
      encoding_(encoding) {}

std::unique_ptr<Intset> Intset::CreateFromRedisBlob(std::string_view blob) {
  if (blob.size() < kBlobHeaderBytes) {
    return nullptr;
  }
  const uint32_t width = LoadLittleEndian32(blob);
  const uint32_t length = LoadLittleEndian32(blob.substr(sizeof(uint32_t)));
  if (width != sizeof(int16_t) && width != sizeof(int32_t) &&
      width != sizeof(int64_t)) {
    return nullptr;
  }
  const std::string_view values = blob.substr(kBlobHeaderBytes);
  if (values.size() % width != 0 || values.size() / width != length) {
    return nullptr;
  }
  const auto encoding = static_cast<EncodingType>(width);
  // Not make_unique, which cannot reach the private constructor.
  std::unique_ptr<Intset> intset(new Intset(length, encoding));
  unsigned char* const contents = intset->contents_.get();
  std::memcpy(contents, values.data(), values.size());
  SwapBetweenHostAndLittleEndian(
      {reinterpret_cast<char*>(contents), values.size()}, width);
  int64_t previous = 0;
  for (uint32_t index = 0; index < length; ++index) {
    const int64_t value = DecodeAt(contents, index, encoding);
    if (index > 0 && value <= previous) {
      return nullptr;
    }
    previous = value;
  }
  return intset;
}

bool Intset::Add(int64_t value) {
  if (ValueEncoding(value) > encoding_) {
    UpgradeAndAdd(value);
    return true;
  }
  uint32_t index = 0;
  if (Search(value, &index)) {
    return false;
  }
  InsertSlot(index);
  Set(index, value);
  return true;
}

int64_t Intset::Get(uint32_t index) const {
  return EncodedValue(index, encoding_);
}

bool Intset::Find(int64_t value) const {
  return ValueEncoding(value) <= encoding_ && Search(value, nullptr);
}

bool Intset::Delete(int64_t value) {
  uint32_t index = 0;
  if (!Search(value, &index)) {
    return false;
  }
  RemoveSlot(index);
  return true;
}

int64_t Intset::Max() const { return Get(length_ - 1); }

int64_t Intset::Min() const { return Get(0); }

Intset::EncodingType Intset::ValueEncoding(int64_t value) {
  if (value < INT32_MIN || value > INT32_MAX) {
    return kInt64;
  }
  if (value < INT16_MIN || value > INT16_MAX) {
    return kInt32;
  }
  return kInt16;
}

size_t Intset::CapacityBytes(uint32_t length, EncodingType encoding) {
  return utils::AllocationSizeClass(static_cast<size_t>(length) * encoding);
}

// Redis' intsetResize reallocates on every insert and delete, which its
// allocator turns into a copy only when the size class changes. Without
// realloc, the same is done here by hand: the values move within the
// allocation while its size class still fits, and otherwise are copied once,
// straight to their final places in a new one.
void Intset::InsertSlot(uint32_t index) {
  const size_t slot_offset = static_cast<size_t>(index) * encoding_;
  const size_t tail_bytes = static_cast<size_t>(length_ - index) * encoding_;
  const size_t new_capacity = CapacityBytes(length_ + 1, encoding_);
  if (new_capacity == CapacityBytes(length_, encoding_)) {
    std::memmove(contents_.get() + slot_offset + encoding_,
                 contents_.get() + slot_offset, tail_bytes);
  } else {
    auto new_contents = AllocateContents(new_capacity);
    std::memcpy(new_contents.get(), contents_.get(), slot_offset);
    std::memcpy(new_contents.get() + slot_offset + encoding_,
                contents_.get() + slot_offset, tail_bytes);
    contents_ = std::move(new_contents);
  }
  ++length_;
}

void Intset::RemoveSlot(uint32_t index) {
  const size_t slot_offset = static_cast<size_t>(index) * encoding_;
  const size_t tail_bytes =
      static_cast<size_t>(length_ - index - 1) * encoding_;
  const size_t new_capacity = CapacityBytes(length_ - 1, encoding_);
  if (new_capacity == CapacityBytes(length_, encoding_)) {
    std::memmove(contents_.get() + slot_offset,
                 contents_.get() + slot_offset + encoding_, tail_bytes);
  } else {
    auto new_contents = AllocateContents(new_capacity);
    std::memcpy(new_contents.get(), contents_.get(), slot_offset);
    std::memcpy(new_contents.get() + slot_offset,
                contents_.get() + slot_offset + encoding_, tail_bytes);
    contents_ = std::move(new_contents);
  }
  --length_;
}

void Intset::UpgradeAndAdd(int64_t value) {
  const EncodingType old_encoding = encoding_;
  const uint32_t old_length = length_;
  auto old_contents = std::move(contents_);
  encoding_ = ValueEncoding(value);
  contents_ = AllocateContents(CapacityBytes(old_length + 1, encoding_));
  const uint32_t prepend = value < 0 ? 1 : 0;
  for (uint32_t index = old_length; index > 0; --index) {
    const uint32_t old_index = index - 1;
    Set(old_index + prepend,
        DecodeAt(old_contents.get(), old_index, old_encoding));
  }
  if (prepend == 1) {
    Set(0, value);
  } else {
    Set(old_length, value);
  }
  length_ = old_length + 1;
}

bool Intset::Search(int64_t value, uint32_t* const index) const {
  if (length_ > 0 && value < EncodedValue(0, encoding_)) {
    if (index != nullptr) {
      *index = 0;
    }
    return false;
  }
  if (length_ > 0 && value > EncodedValue(length_ - 1, encoding_)) {
    if (index != nullptr) {
      *index = length_;
    }
    return false;
  }
  uint32_t min_index = 0;
  uint32_t max_index = length_;
  while (min_index < max_index) {
    const uint32_t mid = min_index + ((max_index - min_index) / 2);
    const int64_t candidate = EncodedValue(mid, encoding_);
    if (candidate == value) {
      if (index != nullptr) {
        *index = mid;
      }
      return true;
    }
    if (candidate < value) {
      min_index = mid + 1;
    } else {
      max_index = mid;
    }
  }
  if (index != nullptr) {
    *index = min_index;
  }
  return false;
}

void Intset::AppendRedisBlob(std::string* const out) const {
  const auto width = static_cast<uint32_t>(encoding_);
  const size_t value_bytes = static_cast<size_t>(length_) * width;
  out->reserve(out->size() + kBlobHeaderBytes + value_bytes);
  AppendLittleEndian32(width, out);
  AppendLittleEndian32(length_, out);
  const size_t values_offset = out->size();
  out->append(reinterpret_cast<const char*>(contents_.get()), value_bytes);
  SwapBetweenHostAndLittleEndian(std::span<char>(*out).subspan(values_offset),
                                 width);
}

int64_t Intset::EncodedValue(uint32_t index, EncodingType encoding) const {
  RS_CHECK(index < length_);
  return DecodeAt(contents_.get(), index, encoding);
}

int64_t Intset::DecodeAt(const unsigned char* contents, uint32_t index,
                         EncodingType encoding) {
  const unsigned char* source =
      contents + (static_cast<size_t>(index) * encoding);
  if (encoding == kInt64) {
    int64_t value64 = 0;
    std::memcpy(&value64, source, sizeof(value64));
    return value64;
  }
  if (encoding == kInt32) {
    int32_t value32 = 0;
    std::memcpy(&value32, source, sizeof(value32));
    return value32;
  }
  int16_t value16 = 0;
  std::memcpy(&value16, source, sizeof(value16));
  return value16;
}

void Intset::Set(uint32_t index, int64_t value) {
  unsigned char* destination =
      contents_.get() + (static_cast<size_t>(index) * encoding_);
  if (encoding_ == kInt64) {
    std::memcpy(destination, &value, sizeof(value));
  } else if (encoding_ == kInt32) {
    const auto value32 = static_cast<int32_t>(value);
    std::memcpy(destination, &value32, sizeof(value32));
  } else {
    const auto value16 = static_cast<int16_t>(value);
    std::memcpy(destination, &value16, sizeof(value16));
  }
}
}  // namespace redis_simple::containers
