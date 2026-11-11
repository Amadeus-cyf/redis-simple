#include "utils/lzf.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace redis_simple::utils {
namespace {
constexpr size_t kMaxLiteral = 32;
// Back references cover 3 to 264 bytes, up to 8192 bytes back.
constexpr size_t kMinMatch = 3;
constexpr size_t kMaxMatch = 264;
constexpr size_t kMaxDistance = size_t{1} << 13;
// The table has 2^n slots, n growing with the input up to this, so small
// inputs only clear a small table.
constexpr int kMinHashBits = 6;
constexpr int kMaxHashBits = 14;

constexpr uint32_t ThreeBytes(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 16) |
         (static_cast<uint32_t>(bytes[1]) << 8) | bytes[2];
}

constexpr size_t HashOf(uint32_t three_bytes, int hash_bits) {
  return (three_bytes * 2654435761U) >> (32 - hash_bits);
}

// Writes the compressed stream, tracking the open literal run.
class Writer {
 public:
  explicit Writer(std::span<char> output)
      : output_(reinterpret_cast<unsigned char*>(output.data())),
        capacity_(output.size()) {}

  bool Literal(unsigned char byte) {
    if (literal_count_ == 0) {
      // Reserve the run's control byte.
      if (size_ >= capacity_) {
        return false;
      }
      literal_control_ = size_++;
    }
    if (size_ >= capacity_) {
      return false;
    }
    output_[size_++] = byte;
    ++literal_count_;
    if (literal_count_ == kMaxLiteral) {
      CloseLiteralRun();
    }
    return true;
  }

  // `length` is kMinMatch..kMaxMatch and `distance` 1..kMaxDistance.
  bool BackReference(size_t length, size_t distance) {
    CloseLiteralRun();
    const size_t encoded_length = length - 2;
    const size_t offset = distance - 1;
    const size_t needed = encoded_length < 7 ? 2 : 3;
    if (capacity_ - size_ < needed) {
      return false;
    }
    if (encoded_length < 7) {
      output_[size_++] =
          static_cast<unsigned char>((encoded_length << 5) | (offset >> 8));
    } else {
      output_[size_++] = static_cast<unsigned char>((7U << 5) | (offset >> 8));
      output_[size_++] = static_cast<unsigned char>(encoded_length - 7);
    }
    output_[size_++] = static_cast<unsigned char>(offset & 0xff);
    return true;
  }

  size_t Finish() {
    CloseLiteralRun();
    return size_;
  }

 private:
  void CloseLiteralRun() {
    if (literal_count_ > 0) {
      output_[literal_control_] =
          static_cast<unsigned char>(literal_count_ - 1);
      literal_count_ = 0;
    }
  }

  unsigned char* output_;
  size_t capacity_;
  size_t size_{};
  size_t literal_control_{};
  size_t literal_count_{};
};
}  // namespace

size_t LzfCompressor::Compress(std::string_view input, std::span<char> output) {
  const auto* const data = reinterpret_cast<const unsigned char*>(input.data());
  const size_t size = input.size();
  if (size == 0 || output.empty() ||
      size > std::numeric_limits<uint32_t>::max() - 1) {
    return 0;
  }
  const int hash_bits =
      std::clamp<int>(std::bit_width(size), kMinHashBits, kMaxHashBits);
  const size_t slots = size_t{1} << hash_bits;
  if (table_.size() < slots) {
    table_.resize(slots);
  }
  std::fill_n(table_.begin(), slots, 0);
  const std::span<uint32_t> table(table_.data(), slots);
  Writer writer(output);
  size_t position = 0;
  while (position + kMinMatch <= size) {
    const uint32_t key = ThreeBytes(data + position);
    uint32_t& slot = table[HashOf(key, hash_bits)];
    const size_t candidate = slot;
    slot = static_cast<uint32_t>(position + 1);
    if (candidate != 0 && position - (candidate - 1) <= kMaxDistance &&
        ThreeBytes(data + candidate - 1) == key) {
      const size_t match = candidate - 1;
      const size_t limit = std::min(kMaxMatch, size - position);
      size_t length = kMinMatch;
      while (length < limit &&
             data[match + length] == data[position + length]) {
        ++length;
      }
      if (!writer.BackReference(length, position - match)) {
        return 0;
      }
      // Index the matched bytes too, so later data can refer into them.
      const size_t end = position + length;
      for (size_t index = position + 1;
           index + kMinMatch <= size && index < end; ++index) {
        table[HashOf(ThreeBytes(data + index), hash_bits)] =
            static_cast<uint32_t>(index + 1);
      }
      position = end;
      continue;
    }
    if (!writer.Literal(data[position])) {
      return 0;
    }
    ++position;
  }
  for (; position < size; ++position) {
    if (!writer.Literal(data[position])) {
      return 0;
    }
  }
  return writer.Finish();
}

bool LzfDecompress(std::string_view input, std::span<char> output) {
  const auto* in = reinterpret_cast<const unsigned char*>(input.data());
  const auto* const in_end = in + input.size();
  auto* const out_begin = reinterpret_cast<unsigned char*>(output.data());
  const size_t capacity = output.size();
  size_t size = 0;
  while (in < in_end) {
    const size_t control = *in++;
    if (control < kMaxLiteral) {
      const size_t length = control + 1;
      if (std::cmp_greater(length, in_end - in) || length > capacity - size) {
        return false;
      }
      std::memcpy(out_begin + size, in, length);
      in += length;
      size += length;
      continue;
    }
    size_t length = control >> 5;
    if (length == 7) {
      if (in == in_end) {
        return false;
      }
      length += *in++;
    }
    if (in == in_end) {
      return false;
    }
    const size_t distance = ((control & 0x1f) << 8) + *in++ + 1;
    length += 2;
    if (distance > size || length > capacity - size) {
      return false;
    }
    // Byte by byte: the source may overlap what is being written.
    const unsigned char* source = out_begin + size - distance;
    for (size_t i = 0; i < length; ++i) {
      out_begin[size + i] = source[i];
    }
    size += length;
  }
  return size == capacity;
}
}  // namespace redis_simple::utils
