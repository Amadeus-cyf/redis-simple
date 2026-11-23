#include "containers/listpack_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace redis_simple::containers {
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

// Encoded sizes whose backlen Redis 8.8 to 8.10 wrote one byte shorter than
// every other release: 7f ff, 7f ff ff, and 7f ff ff ff, where the others
// write 00 ff ff, 00 ff ff ff, and 00 ff ff ff ff.
constexpr std::array<uint64_t, 3> kBacklenLimits = {16383, 2097151, 268435455};

// The number of bytes the backlen field takes for an element whose encoded
// size is `length`, as Redis' lpEncodeBacklenBytes: a size equal to one of
// kBacklenLimits takes a byte more than it needs.
size_t BacklenBytes(uint64_t length) {
  if (length <= 127) {
    return 1;
  }
  if (length < kBacklenLimits[0]) {
    return 2;
  }
  if (length < kBacklenLimits[1]) {
    return 3;
  }
  if (length < kBacklenLimits[2]) {
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
// What an entry's first bytes say about it.
struct EntryHeader {
  // False for a first byte that is no encoding.
  bool valid{true};
  // The encoding's own bytes: the type byte and any length or integer bytes.
  size_t header_bytes{1};
  // The string bytes that follow; zero for an integer.
  size_t payload_bytes{};
  bool is_integer{};
  int64_t integer{};
};

// Decodes the entry at `entry`, of which `available` bytes are in the
// buffer, as Redis' lpCurrentEncodedSizeUnsafe and lpGetIntegerIfValid do.
// A header longer than `available` is reported by its size alone; the
// caller rejects it.
EntryHeader DecodeEntryHeader(const unsigned char* const entry,
                              size_t available) {
  const unsigned char type = entry[0];
  EntryHeader header;
  if ((type & 0x80) == 0) {
    // A 7-bit unsigned integer in the type byte.
    header.is_integer = true;
    header.integer = type & 0x7f;
  } else if ((type & 0xc0) == 0x80) {
    // A string with a 6-bit length.
    header.payload_bytes = type & 0x3f;
  } else if ((type & 0xe0) == 0xc0) {
    // A 13-bit signed integer.
    header.header_bytes = 2;
    if (available >= 2) {
      header.is_integer = true;
      header.integer =
          SignExtend((static_cast<uint64_t>(type & 0x1f) << 8) | entry[1], 13);
    }
  } else if ((type & 0xf0) == 0xe0) {
    // A string with a 12-bit length.
    header.header_bytes = 2;
    if (available >= 2) {
      header.payload_bytes = (static_cast<size_t>(type & 0x0f) << 8) | entry[1];
    }
  } else if (type == 0xf0) {
    // A string with a 32-bit length.
    header.header_bytes = 5;
    if (available >= 5) {
      header.payload_bytes = LoadLittleEndian(entry + 1, 4);
    }
  } else if (type >= 0xf1 && type <= 0xf4) {
    // A 16-, 24-, 32-, or 64-bit signed integer.
    static constexpr std::array<size_t, 4> kWidths = {2, 3, 4, 8};
    const size_t width = kWidths[type - 0xf1];
    header.header_bytes = 1 + width;
    if (available >= header.header_bytes) {
      header.is_integer = true;
      header.integer = SignExtend(LoadLittleEndian(entry + 1, width),
                                  static_cast<unsigned>(width * 8));
    }
  } else {
    // 0xf5 to 0xfe are not encodings, and 0xff may only be the last byte.
    header.valid = false;
  }
  return header;
}

// Whether the `available` bytes at `backlen` start with the shorter backlen
// Redis 8.8 to 8.10 wrote for an element of encoded size `encoded`.
bool IsCompactBacklen(const unsigned char* const backlen, size_t available,
                      uint64_t encoded) {
  if (std::ranges::find(kBacklenLimits, encoded) == kBacklenLimits.end()) {
    return false;
  }
  // 7f followed by ff bytes, one byte shorter than the usual width. The
  // whole form must match, so corruption is never "repaired".
  const size_t width = BacklenBytes(encoded) - 1;
  if (available < width || backlen[0] != 0x7f) {
    return false;
  }
  return std::all_of(backlen + 1, backlen + width,
                     [](unsigned char byte) { return byte == 0xff; });
}

// Appends Redis' lpEncodeBacklen of `length`: the most significant 7 bits
// first, and the high bit set on every later byte.
void AppendBacklen(std::string* const out, uint64_t length) {
  const size_t width = BacklenBytes(length);
  for (size_t i = 0; i < width; ++i) {
    auto byte =
        static_cast<unsigned char>((length >> (7 * (width - 1 - i))) & 127);
    if (i > 0) {
      byte |= 128;
    }
    out->push_back(static_cast<char>(byte));
  }
}
}  // namespace

bool WidenCompactBacklens(std::string* const listpack) {
  // The smallest listpack that can hold an element of encoded size 16383:
  // the header, the element, a two-byte backlen, and the terminator.
  constexpr size_t kMinBytes = kHeaderBytes + kBacklenLimits[0] + 2 + 1;
  const size_t size = listpack->size();
  if (size < kMinBytes) {
    return false;
  }
  const auto* const bytes =
      reinterpret_cast<const unsigned char*>(listpack->data());
  if (LoadLittleEndian(bytes, 4) != size || bytes[size - 1] != kEnd) {
    return false;
  }
  const size_t end = size - 1;
  // The first pass checks every entry, taking each backlen in whichever
  // width it has, and counts the short ones.
  size_t compact = 0;
  size_t offset = kHeaderBytes;
  while (offset < end) {
    const size_t available = end - offset;
    const EntryHeader header = DecodeEntryHeader(bytes + offset, available);
    if (!header.valid || header.header_bytes >= available ||
        header.payload_bytes >= available - header.header_bytes) {
      return false;
    }
    const size_t encoded = header.header_bytes + header.payload_bytes;
    size_t backlen_bytes = BacklenBytes(encoded);
    if (IsCompactBacklen(bytes + offset + encoded, available - encoded,
                         encoded)) {
      --backlen_bytes;
      ++compact;
    }
    if (backlen_bytes > available - encoded) {
      return false;
    }
    offset += encoded + backlen_bytes;
  }
  if (offset != end || compact == 0 ||
      size + compact > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  // The second pass copies the entries, writing the short backlens in full.
  std::string widened;
  widened.reserve(size + compact);
  widened.append(listpack->data(), kHeaderBytes);
  offset = kHeaderBytes;
  while (offset < end) {
    const EntryHeader header = DecodeEntryHeader(bytes + offset, end - offset);
    const size_t encoded = header.header_bytes + header.payload_bytes;
    widened.append(listpack->data() + offset, encoded);
    size_t backlen_bytes = BacklenBytes(encoded);
    if (IsCompactBacklen(bytes + offset + encoded, end - offset - encoded,
                         encoded)) {
      --backlen_bytes;
      AppendBacklen(&widened, encoded);
    } else {
      widened.append(listpack->data() + offset + encoded, backlen_bytes);
    }
    offset += encoded + backlen_bytes;
  }
  widened.push_back(static_cast<char>(kEnd));
  const size_t total = widened.size();
  for (size_t i = 0; i < 4; ++i) {
    widened[i] = static_cast<char>((total >> (8 * i)) & 0xff);
  }
  *listpack = std::move(widened);
  return true;
}

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
  const EntryHeader header = DecodeEntryHeader(entry, available);
  if (!header.valid || header.header_bytes > available ||
      header.payload_bytes > available - header.header_bytes) {
    offset_ = 0;
    return Status::kCorrupt;
  }
  const size_t encoded_bytes = header.header_bytes + header.payload_bytes;
  const size_t backlen_bytes = BacklenBytes(encoded_bytes);
  if (backlen_bytes > available - encoded_bytes ||
      DecodeBacklen(entry + encoded_bytes + backlen_bytes - 1, backlen_bytes) !=
          encoded_bytes) {
    offset_ = 0;
    return Status::kCorrupt;
  }
  *element = {};
  element->is_integer = header.is_integer;
  element->integer = header.integer;
  if (!header.is_integer) {
    element->string = std::string_view(
        reinterpret_cast<const char*>(entry + header.header_bytes),
        header.payload_bytes);
  }
  offset_ += encoded_bytes + backlen_bytes;
  ++count_;
  return Status::kElement;
}
}  // namespace redis_simple::containers
