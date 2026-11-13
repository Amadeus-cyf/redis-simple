#include "memory/intset.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "utils/check.h"

namespace redis_simple::memory {
IntSet::IntSet()
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): compact byte storage
    : contents_(std::make_unique<unsigned char[]>(
          static_cast<size_t>(kInitSize) * kInt16)),
      length_(0),
      encoding_(kInt16) {}
bool IntSet::Add(int64_t value) {
  uint32_t value_encode = ValueEncoding(value);
  if (value_encode > encoding_) {
    UpgradeAndAdd(value);
    return true;
  }
  uint32_t index = 0;
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

int64_t IntSet::Get(uint32_t index) const {
  return EncodedValue(index, encoding_);
}

bool IntSet::Find(int64_t value) const {
  EncodingType encoding = ValueEncoding(value);
  return encoding <= encoding_ && Search(value, nullptr);
}

bool IntSet::Delete(int64_t value) {
  uint32_t index = 0;
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

void IntSet::Resize(uint32_t length) {
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
  const uint32_t old_length = length_;
  auto old_contents = std::move(contents_);
  encoding_ = ValueEncoding(value);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): compact byte storage
  contents_ = std::make_unique<unsigned char[]>(
      static_cast<size_t>(old_length + 1) * encoding_);
  const uint32_t prepend = value < 0 ? 1 : 0;
  for (uint32_t index = old_length; index > 0; --index) {
    const uint32_t old_index = index - 1;
    int64_t old_value = 0;
    const unsigned char* source =
        old_contents.get() + (static_cast<size_t>(old_index * old_encoding));
    if (old_encoding == kInt64) {
      std::memcpy(&old_value, source, sizeof(old_value));
    } else if (old_encoding == kInt32) {
      int32_t value32 = 0;
      std::memcpy(&value32, source, sizeof(value32));
      old_value = value32;
    } else {
      int16_t value16 = 0;
      std::memcpy(&value16, source, sizeof(value16));
      old_value = value16;
    }
    Set(old_index + prepend, old_value);
  }
  if (prepend == 1) {
    Set(0, value);
  } else {
    Set(old_length, value);
  }
  length_ = old_length + 1;
}

bool IntSet::Search(int64_t value, uint32_t* const index) const {
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

void IntSet::AppendRedisBlob(std::string* const out) const {
  const auto append = [out](uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      out->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
  };
  const auto width = static_cast<size_t>(encoding_);
  out->reserve(out->size() + 8 + (width * length_));
  append(width, 4);
  append(length_, 4);
  for (uint32_t index = 0; index < length_; ++index) {
    append(static_cast<uint64_t>(EncodedValue(index, encoding_)), width);
  }
}

int64_t IntSet::EncodedValue(uint32_t index, EncodingType encoding) const {
  RS_CHECK(index < length_);
  const unsigned char* source =
      contents_.get() + (static_cast<size_t>(index * encoding));
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

void IntSet::Set(uint32_t index, int64_t value) {
  unsigned char* destination =
      contents_.get() + (static_cast<size_t>(index * encoding_));
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

void IntSet::MoveTail(uint32_t from, uint32_t to) {
  const size_t bytes = static_cast<size_t>(length_ - from) * encoding_;
  const unsigned char* source =
      contents_.get() + (static_cast<size_t>(from * encoding_));
  unsigned char* destination =
      contents_.get() + (static_cast<size_t>(to * encoding_));
  std::memmove(destination, source, bytes);
}
}  // namespace redis_simple::memory
