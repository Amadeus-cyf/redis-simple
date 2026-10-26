#include "memory/intset.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include "utils/check.h"

namespace redis_simple::in_memory {
IntSet::IntSet()
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): compact byte storage
    : contents_(std::make_unique<unsigned char[]>(
          static_cast<size_t>(kInitSize) * kInt16)),
      length_(0),
      encoding_(kInt16) {}
bool IntSet::Add(int64_t value) {
  unsigned int value_encode = ValueEncoding(value);
  if (value_encode > encoding_) {
    UpgradeAndAdd(value);
    return true;
  }
  unsigned int index = 0;
  if (Search(value, &index)) {
    return false;
  }
  Resize(length_ + 1);
  if (index < length_) {
    MoveTail(index, index + 1);
  }
  Set(index, value);
  ++length_;
  return true;
}

int64_t IntSet::Get(unsigned int index) const {
  return EncodedValue(index, encoding_);
}

bool IntSet::Find(int64_t value) const {
  EncodingType encoding = ValueEncoding(value);
  return encoding <= encoding_ && Search(value, nullptr);
}

bool IntSet::Remove(int64_t value) {
  unsigned int index = 0;
  bool exist = Search(value, &index);
  if (!exist) {
    return false;
  }
  MoveTail(index + 1, index);
  Resize(length_ - 1);
  --length_;
  return true;
}

int64_t IntSet::Max() const { return Get(length_ - 1); }

int64_t IntSet::Min() const { return Get(0); }

IntSet::EncodingType IntSet::ValueEncoding(int64_t value) {
  if (value < INT32_MIN || value > INT32_MAX) {
    return kInt64;
  }
  if (value < INT16_MIN || value > INT16_MAX) {
    return kInt32;
  }
  return kInt16;
}

void IntSet::Resize(unsigned int length) {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): compact byte storage
  auto new_contents = std::make_unique<unsigned char[]>(
      static_cast<size_t>(length) * encoding_);
  const size_t copy_bytes =
      static_cast<size_t>(std::min(length_, length)) * encoding_;
  std::memcpy(new_contents.get(), contents_.get(), copy_bytes);
  contents_ = std::move(new_contents);
}

void IntSet::UpgradeAndAdd(int64_t value) {
  const EncodingType old_encoding = encoding_;
  const unsigned int old_length = length_;
  auto old_contents = std::move(contents_);
  encoding_ = ValueEncoding(value);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): compact byte storage
  contents_ = std::make_unique<unsigned char[]>(
      static_cast<size_t>(old_length + 1) * encoding_);
  const unsigned int prepend = value < 0 ? 1 : 0;
  for (unsigned int idx = old_length; idx > 0; --idx) {
    const unsigned int old_idx = idx - 1;
    int64_t old_value = 0;
    const unsigned char* src =
        old_contents.get() + (static_cast<size_t>(old_idx * old_encoding));
    if (old_encoding == kInt64) {
      std::memcpy(&old_value, src, sizeof(old_value));
    } else if (old_encoding == kInt32) {
      int32_t value32 = 0;
      std::memcpy(&value32, src, sizeof(value32));
      old_value = value32;
    } else {
      int16_t value16 = 0;
      std::memcpy(&value16, src, sizeof(value16));
      old_value = value16;
    }
    Set(old_idx + prepend, old_value);
  }
  if (prepend == 1) {
    Set(0, value);
  } else {
    Set(old_length, value);
  }
  length_ = old_length + 1;
}

bool IntSet::Search(int64_t value, unsigned int* const index) const {
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
  unsigned int min_idx = 0;
  unsigned int max_idx = length_;
  while (min_idx < max_idx) {
    const unsigned int mid = min_idx + ((max_idx - min_idx) / 2);
    int64_t val = EncodedValue(mid, encoding_);
    if (val == value) {
      if (index != nullptr) {
        *index = mid;
      }
      return true;
    }
    if (val < value) {
      min_idx = mid + 1;
    } else {
      max_idx = mid;
    }
  }
  if (index != nullptr) {
    *index = min_idx;
  }
  return false;
}

int64_t IntSet::EncodedValue(unsigned int index, EncodingType encoding) const {
  RS_CHECK(index < length_);
  const unsigned char* src =
      contents_.get() + (static_cast<size_t>(index * encoding));
  if (encoding == kInt64) {
    int64_t v64 = 0;
    std::memcpy(&v64, src, sizeof(v64));
    return v64;
  }
  if (encoding == kInt32) {
    int32_t v32 = 0;
    std::memcpy(&v32, src, sizeof(v32));
    return v32;
  }
  int16_t v16 = 0;
  std::memcpy(&v16, src, sizeof(v16));
  return v16;
}

void IntSet::Set(unsigned int index, int64_t value) {
  unsigned char* dst =
      contents_.get() + (static_cast<size_t>(index * encoding_));
  if (encoding_ == kInt64) {
    std::memcpy(dst, &value, sizeof(value));
  } else if (encoding_ == kInt32) {
    const auto value32 = static_cast<int32_t>(value);
    std::memcpy(dst, &value32, sizeof(value32));
  } else {
    const auto value16 = static_cast<int16_t>(value);
    std::memcpy(dst, &value16, sizeof(value16));
  }
}

void IntSet::MoveTail(unsigned int from, unsigned int to) {
  const size_t bytes = static_cast<size_t>(length_ - from) * encoding_;
  const unsigned char* src =
      contents_.get() + (static_cast<size_t>(from * encoding_));
  unsigned char* dst = contents_.get() + (static_cast<size_t>(to * encoding_));
  std::memmove(dst, src, bytes);
}
}  // namespace redis_simple::in_memory
