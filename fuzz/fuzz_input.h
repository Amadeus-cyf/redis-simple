#ifndef REDIS_SIMPLE_FUZZ_FUZZ_INPUT_H_
#define REDIS_SIMPLE_FUZZ_FUZZ_INPUT_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace redis_simple::fuzz {
class FuzzInput {
 public:
  FuzzInput(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool HasData() const { return offset_ < size_; }
  size_t Remaining() const { return size_ - offset_; }

  uint8_t ReadByte() { return HasData() ? data_[offset_++] : uint8_t{0}; }

  size_t ReadIndex(size_t bound) {
    const uint16_t value =
        (static_cast<uint16_t>(ReadByte()) << 8) | ReadByte();
    return bound == 0 ? 0 : value % bound;
  }

  int64_t ReadInt64() {
    constexpr std::array<int64_t, 8> kBoundaryValues = {
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int32_t>::min(),
        -4096,
        -1,
        0,
        127,
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int64_t>::max(),
    };
    const uint8_t selector = ReadByte();
    if (selector < kBoundaryValues.size()) {
      return kBoundaryValues[selector];
    }

    uint64_t magnitude = 0;
    for (size_t i = 0; i < 7; ++i) {
      magnitude = (magnitude << 8) | ReadByte();
    }
    const auto value = static_cast<int64_t>(magnitude);
    return (selector & 1U) == 0 ? value : -value;
  }

  double ReadScore() {
    constexpr std::array<double, 8> kBoundaryValues = {
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::lowest(),
        -1.0,
        -0.0,
        0.0,
        1.0,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
    };
    const uint8_t selector = ReadByte();
    if (selector < kBoundaryValues.size()) {
      return kBoundaryValues[selector];
    }
    if (selector == kBoundaryValues.size()) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    constexpr double kScoreScale = 100.0;
    return static_cast<double>(ReadInt64()) / kScoreScale;
  }

  std::string ReadValue(size_t max_bytes) {
    const uint16_t selector =
        (static_cast<uint16_t>(ReadByte()) << 8) | ReadByte();
    const size_t available = std::min(Remaining(), max_bytes);
    const size_t length = selector % (available + 1);
    std::string value;
    value.reserve(length);
    if (length > 0) {
      value.append(reinterpret_cast<const char*>(data_ + offset_), length);
      offset_ += length;
    }
    return value;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_{};
};

inline void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

inline std::string PadToSize(std::string value, size_t size, char fill = 'x') {
  value.resize(std::max(value.size(), size), fill);
  return value;
}

template <typename T>
const T& ValueOrAbort(const std::optional<T>& value) {
  if (!value.has_value()) {
    std::abort();
  }
  return *value;
}
}  // namespace redis_simple::fuzz

#endif  // REDIS_SIMPLE_FUZZ_FUZZ_INPUT_H_
