#include "memory/listpack_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace redis_simple::memory {
namespace {
constexpr size_t kHeaderBytes = 6;
constexpr unsigned char kEnd = 0xff;
constexpr uint16_t kUnknownCount = std::numeric_limits<uint16_t>::max();

uint64_t LoadLittleEndian(const unsigned char* bytes, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) {
    value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
  }
  return value;
}

// Two's complement of a `bits`-wide field.
int64_t SignExtend(uint64_t value, unsigned bits) {
  if (bits > 0 && bits < 64 && (value >> (bits - 1)) != 0) {
    return static_cast<int64_t>(value) - static_cast<int64_t>(1ULL << bits);
  }
  return static_cast<int64_t>(value);
}

// Redis' lpEncodeBacklenBytes.
size_t BacklenBytes(uint64_t length) {
  if (length <= 127) {
    return 1;
  }
  if (length <= 16383) {
    return 2;
  }
  if (length <= 2097151) {
    return 3;
  }
  if (length <= 268435455) {
    return 4;
  }
  return 5;
}

// Redis' lpDecodeBacklen, reading backward from `last`; returns UINT64_MAX
// for more than five bytes.
uint64_t DecodeBacklen(const unsigned char* last, size_t available) {
  uint64_t value = 0;
  unsigned shift = 0;
  for (size_t i = 0; i < available && i < 5; ++i) {
    const unsigned char byte = *(last - i);
    value |= static_cast<uint64_t>(byte & 127) << shift;
    if ((byte & 128) == 0) {
      return value;
    }
    shift += 7;
  }
  return std::numeric_limits<uint64_t>::max();
}
}  // namespace

bool ListpackReader::ReadHeader() {
  header_read_ = true;
  if (blob_.size() < kHeaderBytes + 1) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const unsigned char*>(blob_.data());
  if (LoadLittleEndian(bytes, 4) != blob_.size() ||
      bytes[blob_.size() - 1] != kEnd) {
    return false;
  }
  header_count_ = static_cast<uint16_t>(LoadLittleEndian(bytes + 4, 2));
  offset_ = kHeaderBytes;
  return true;
}

ListpackReader::Status ListpackReader::Next(Element* const element) {
  if (!header_read_ && !ReadHeader()) {
    return Status::kCorrupt;
  }
  if (offset_ == 0) {
    // The header was invalid, or a previous call found corruption.
    return Status::kCorrupt;
  }
  const auto* const bytes =
      reinterpret_cast<const unsigned char*>(blob_.data());
  // The terminator is the last byte, so entries end before it.
  const size_t limit = blob_.size() - 1;
  if (offset_ == limit) {
    if (header_count_ != kUnknownCount && header_count_ != count_) {
      offset_ = 0;
      return Status::kCorrupt;
    }
    return Status::kEnd;
  }
  const unsigned char* const entry = bytes + offset_;
  const size_t available = limit - offset_;
  const unsigned char type = entry[0];
  // The encoding's header bytes (type and length), and the payload's size.
  size_t header_bytes = 1;
  size_t payload_bytes = 0;
  *element = {};
  if ((type & 0x80) == 0) {
    element->is_integer = true;
    element->integer = type & 0x7f;
  } else if ((type & 0xc0) == 0x80) {
    payload_bytes = type & 0x3f;
  } else if ((type & 0xe0) == 0xc0) {
    header_bytes = 2;
    if (available >= 2) {
      element->is_integer = true;
      element->integer =
          SignExtend((static_cast<uint64_t>(type & 0x1f) << 8) | entry[1], 13);
    }
  } else if ((type & 0xf0) == 0xe0) {
    header_bytes = 2;
    if (available >= 2) {
      payload_bytes = (static_cast<size_t>(type & 0x0f) << 8) | entry[1];
    }
  } else if (type == 0xf0) {
    header_bytes = 5;
    if (available >= 5) {
      payload_bytes = LoadLittleEndian(entry + 1, 4);
    }
  } else if (type >= 0xf1 && type <= 0xf4) {
    static constexpr std::array<size_t, 4> kWidths = {2, 3, 4, 8};
    const size_t width = kWidths[type - 0xf1];
    header_bytes = 1 + width;
    if (available >= header_bytes) {
      element->is_integer = true;
      element->integer = SignExtend(LoadLittleEndian(entry + 1, width),
                                    static_cast<unsigned>(width * 8));
    }
  } else {
    // 0xf5 to 0xfe are not encodings, and 0xff may only be the last byte.
    offset_ = 0;
    return Status::kCorrupt;
  }
  if (header_bytes > available || payload_bytes > available - header_bytes) {
    offset_ = 0;
    return Status::kCorrupt;
  }
  const size_t encoded_bytes = header_bytes + payload_bytes;
  const size_t backlen_bytes = BacklenBytes(encoded_bytes);
  if (backlen_bytes > available - encoded_bytes ||
      DecodeBacklen(entry + encoded_bytes + backlen_bytes - 1, backlen_bytes) !=
          encoded_bytes) {
    offset_ = 0;
    return Status::kCorrupt;
  }
  if (!element->is_integer) {
    element->string = std::string_view(
        reinterpret_cast<const char*>(entry + header_bytes), payload_bytes);
  }
  offset_ += encoded_bytes + backlen_bytes;
  ++count_;
  return Status::kElement;
}
}  // namespace redis_simple::memory
