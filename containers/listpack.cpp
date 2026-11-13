#include "containers/listpack.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "utils/check.h"
#include "utils/strings.h"

namespace redis_simple::containers {
namespace {
// Multi-byte fields are little-endian, as in Redis' listpack: the header's
// total size and element count, 32-bit string lengths, and integers wider
// than 13 bits.
template <size_t Width>
uint64_t LoadLittleEndian(const unsigned char* bytes) {
  // Whole words with memcpy, which compiles to one load; the 3-byte width
  // goes byte by byte.
  if constexpr (Width == 2 || Width == 4 || Width == 8) {
    using Word =
        std::conditional_t<Width == 2, uint16_t,
                           std::conditional_t<Width == 4, uint32_t, uint64_t>>;
    Word word = 0;
    std::memcpy(&word, bytes, Width);
    if constexpr (std::endian::native == std::endian::big) {
      word = std::byteswap(word);
    }
    return word;
  } else {
    uint64_t value = 0;
    for (size_t i = 0; i < Width; ++i) {
      value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
  }
}

template <size_t Width>
void StoreLittleEndian(uint64_t value, unsigned char* bytes) {
  if constexpr (Width == 2 || Width == 4 || Width == 8) {
    using Word =
        std::conditional_t<Width == 2, uint16_t,
                           std::conditional_t<Width == 4, uint32_t, uint64_t>>;
    auto word = static_cast<Word>(value);
    if constexpr (std::endian::native == std::endian::big) {
      word = std::byteswap(word);
    }
    std::memcpy(bytes, &word, Width);
  } else {
    for (size_t i = 0; i < Width; ++i) {
      bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    }
  }
}
}  // namespace

Listpack::Listpack()
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): packed byte storage
    : listpack_(std::make_unique<unsigned char[]>(kListpackHeaderSize + 1)),
      capacity_(kListpackHeaderSize + 1) {
  SetTotalBytes(kListpackHeaderSize + 1);
  SetNumOfElements(0);
  listpack_[kListpackHeaderSize] = kListpackEof;
}

std::unique_ptr<Listpack> Listpack::CreateFromBytes(std::string_view bytes) {
  RS_CHECK(bytes.size() > kListpackHeaderSize &&
           bytes.size() <= kListpackMaxSafetySize &&
           static_cast<unsigned char>(bytes.back()) == kListpackEof);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): packed byte storage
  std::unique_ptr<unsigned char[]> storage(new unsigned char[bytes.size()]);
  std::memcpy(storage.get(), bytes.data(), bytes.size());
  std::unique_ptr<Listpack> listpack(
      new Listpack(std::move(storage), bytes.size()));
  RS_CHECK(listpack->TotalBytes() == bytes.size());
  return listpack;
}

unsigned char* Listpack::Get(size_t offset, size_t* const length) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListpackHeaderSize && offset < listpack_bytes);
  if (offset == listpack_bytes - 1) {
    return nullptr;
  }
  EncodingType encoding_type = EncodingAt(offset);
  if (IsString(encoding_type)) {
    return StringAt(offset, length, encoding_type);
  }
  return IntegerAt(offset, int_buffer_.data(), length, nullptr, encoding_type);
}

std::optional<std::string> Listpack::Get(size_t offset) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListpackHeaderSize && offset < listpack_bytes);
  if (offset == listpack_bytes - 1) {
    return std::nullopt;
  }

  size_t length = 0;
  std::array<unsigned char, kListpackIntBufferSize> int_buffer{};
  EncodingType encoding_type = EncodingAt(offset);
  const unsigned char* buffer =
      IsString(encoding_type) ? StringAt(offset, &length, encoding_type)
                              : IntegerAt(offset, int_buffer.data(), &length,
                                          nullptr, encoding_type);
  return std::string(reinterpret_cast<const char*>(buffer), length);
}

std::optional<int64_t> Listpack::IntegerAt(size_t offset) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListpackHeaderSize && offset < listpack_bytes);
  if (offset == listpack_bytes - 1) {
    return std::nullopt;
  }
  EncodingType encoding_type = EncodingAt(offset);
  if (IsString(encoding_type)) {
    return std::nullopt;
  }
  int64_t value = 0;
  IntegerAt(offset, nullptr, nullptr, &value, encoding_type);
  return value;
}

std::optional<size_t> Listpack::Find(std::string_view value) const {
  return FindAndSkip(value, 0);
}

std::optional<size_t> Listpack::FindAndSkip(std::string_view value,
                                            size_t skip) const {
  // As Redis' lpFind does: decode each compared element once, reading its
  // size along with its value, and step over skipped elements by size alone.
  // An element is stored as an integer exactly when its text parses as one,
  // so parse `value` once and compare integers as numbers, without
  // formatting them, and strings only when `value` is not an integer.
  int64_t value_integer = 0;
  const bool value_is_integer = utils::ToInt64(value, &value_integer);
  size_t offset = kListpackHeaderSize;
  size_t skip_count = 0;
  while (listpack_[offset] != kListpackEof) {
    const unsigned char* const buffer = listpack_.get() + offset;
    if (skip_count > 0) {
      --skip_count;
      // Short strings and small integers, the common values, have one-byte
      // encodings and backlens.
      if ((buffer[0] & EncodingTypeMask::kType6BitStrMask) ==
          EncodingType::k6BitString) {
        offset += 1 + (buffer[0] & 0x3f) + 1;
      } else if ((buffer[0] & EncodingTypeMask::kType7BitUintMask) ==
                 EncodingType::k7BitUnsignedInteger) {
        offset += 2;
      } else {
        offset = Skip(offset);
      }
      continue;
    }
    skip_count = skip;
    // Short strings, the common case, need no encoding lookup.
    if ((buffer[0] & EncodingTypeMask::kType6BitStrMask) ==
        EncodingType::k6BitString) {
      const size_t length = buffer[0] & 0x3f;
      if (!value_is_integer &&
          std::string_view(reinterpret_cast<const char*>(buffer + 1), length) ==
              value) {
        return offset;
      }
      offset += 1 + length + 1;
      continue;
    }
    const EncodingType encoding_type = EncodingAt(offset);
    if (IsString(encoding_type)) {
      size_t length = 0;
      const unsigned char* const data =
          StringAt(offset, &length, encoding_type);
      if (!value_is_integer &&
          std::string_view(reinterpret_cast<const char*>(data), length) ==
              value) {
        return offset;
      }
    } else if (value_is_integer) {
      size_t length = 0;
      int64_t integer = 0;
      IntegerAt(offset, nullptr, &length, &integer, encoding_type);
      if (integer == value_integer) {
        return offset;
      }
    }
    const size_t encoded_bytes = EncodedBytes(offset, encoding_type);
    offset += encoded_bytes + BacklenBytes(encoded_bytes);
  }
  return std::nullopt;
}

bool Listpack::Append(std::string_view element_string) {
  uint32_t listpack_bytes = TotalBytes();
  return Insert(listpack_bytes - 1, Position::kInsertBefore, &element_string,
                nullptr);
}

bool Listpack::Append(int64_t element_integer) {
  uint32_t listpack_bytes = TotalBytes();
  return Insert(listpack_bytes - 1, Position::kInsertBefore, nullptr,
                &element_integer);
}

bool Listpack::Prepend(std::string_view element_string) {
  return Insert(kListpackHeaderSize, Position::kInsertBefore, &element_string,
                nullptr);
}

bool Listpack::Prepend(int64_t element_integer) {
  return Insert(kListpackHeaderSize, Position::kInsertBefore, nullptr,
                &element_integer);
}

bool Listpack::Insert(size_t offset, std::string_view element_string) {
  return Insert(offset, Position::kInsertBefore, &element_string, nullptr);
}

bool Listpack::Insert(size_t offset, int64_t element_integer) {
  return Insert(offset, Position::kInsertBefore, nullptr, &element_integer);
}

bool Listpack::Replace(size_t offset, std::string_view element_string) {
  return Insert(offset, Position::kReplace, &element_string, nullptr);
}

bool Listpack::Replace(size_t offset, int64_t element_integer) {
  return Insert(offset, Position::kReplace, nullptr, &element_integer);
}

bool Listpack::BatchAppend(const std::vector<ListpackEntry>& entries) {
  return BatchInsert(TotalBytes() - 1, Position::kInsertBefore, entries.data(),
                     entries.size());
}

bool Listpack::BatchAppend(std::initializer_list<ListpackEntry> entries) {
  return BatchInsert(TotalBytes() - 1, Position::kInsertBefore, entries.begin(),
                     entries.size());
}

bool Listpack::BatchPrepend(const std::vector<ListpackEntry>& entries) {
  return BatchInsert(kListpackHeaderSize, Position::kInsertBefore,
                     entries.data(), entries.size());
}

bool Listpack::BatchInsert(size_t offset,
                           const std::vector<ListpackEntry>& entries) {
  return BatchInsert(offset, Position::kInsertBefore, entries.data(),
                     entries.size());
}

std::optional<size_t> Listpack::OffsetAt(size_t index) const {
  std::optional<size_t> offset = First();
  size_t current_index = 0;
  while (offset.has_value() && current_index < index) {
    offset = Next(*offset);
    ++current_index;
  }
  return offset;
}

void Listpack::Delete(size_t offset) { DeleteRange(offset, 1); }

size_t Listpack::DeleteRange(size_t offset, size_t count) {
  uint32_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListpackHeaderSize && offset < listpack_bytes);
  if (offset == listpack_bytes - 1 || count == 0) {
    return 0;
  }
  size_t end = offset;
  size_t deleted = 0;
  while (deleted < count && end < listpack_bytes - 1) {
    end = Skip(end);
    ++deleted;
  }
  const size_t deleted_bytes = end - offset;
  const size_t new_listpack_bytes = listpack_bytes - deleted_bytes;
  const uint16_t element_count = ElementCount();
  std::memmove(listpack_.get() + offset, listpack_.get() + end,
               listpack_bytes - end);
  // Shrink after memmove so the source range remains valid.
  Realloc(new_listpack_bytes);
  SetTotalBytes(new_listpack_bytes);
  if (element_count != kListpackUnknownElementCount) {
    SetNumOfElements(element_count - deleted);
  }
  return deleted;
}

size_t Listpack::DeleteMatching(std::string_view value, size_t limit,
                                bool from_tail) {
  const bool limited = limit != 0;
  // Byte spans [first, second) of matching elements, merged when adjacent.
  std::vector<std::pair<size_t, size_t>> spans;
  size_t match_count = 0;
  const auto add_span = [&spans](size_t begin, size_t end) {
    if (!spans.empty() && spans.back().second == begin) {
      spans.back().second = end;
    } else {
      spans.emplace_back(begin, end);
    }
  };
  if (from_tail && limited) {
    std::array<unsigned char, kListpackIntBufferSize> int_buffer{};
    for (std::optional<size_t> offset = Last();
         offset.has_value() && match_count < limit; offset = Prev(*offset)) {
      const DecodedEntry entry = DecodeEntry(*offset, int_buffer.data());
      if (ValueView(entry) == value) {
        if (!spans.empty() && spans.back().first == entry.next) {
          spans.back().first = *offset;
        } else {
          spans.emplace_back(*offset, entry.next);
        }
        ++match_count;
      }
    }
    std::ranges::reverse(spans);
  } else {
    std::array<unsigned char, kListpackIntBufferSize> int_buffer{};
    size_t offset = kListpackHeaderSize;
    while (listpack_[offset] != kListpackEof &&
           (!limited || match_count < limit)) {
      const DecodedEntry entry = DecodeEntry(offset, int_buffer.data());
      if (ValueView(entry) == value) {
        add_span(offset, entry.next);
        ++match_count;
      }
      offset = entry.next;
    }
  }
  if (spans.empty()) {
    return 0;
  }

  // Slide each kept run down over the deleted spans in one pass. Backlens
  // describe their own element, so moved elements stay valid.
  const size_t listpack_bytes = TotalBytes();
  size_t write = spans.front().first;
  for (size_t i = 0; i < spans.size(); ++i) {
    const size_t keep_begin = spans[i].second;
    // The last run includes the EOF byte.
    const size_t keep_end =
        i + 1 < spans.size() ? spans[i + 1].first : listpack_bytes;
    std::memmove(listpack_.get() + write, listpack_.get() + keep_begin,
                 keep_end - keep_begin);
    write += keep_end - keep_begin;
  }
  const uint16_t element_count = ElementCount();
  // Shrink after the moves so every source range remains valid.
  Realloc(write);
  SetTotalBytes(write);
  if (element_count != kListpackUnknownElementCount) {
    SetNumOfElements(static_cast<uint16_t>(element_count - match_count));
  }
  return match_count;
}

std::optional<size_t> Listpack::First() const {
  size_t listpack_bytes = TotalBytes();
  if (listpack_bytes <= Listpack::kListpackHeaderSize + 1) {
    return std::nullopt;
  }
  return Listpack::kListpackHeaderSize;
}

std::optional<size_t> Listpack::Last() const {
  size_t listpack_bytes = TotalBytes();
  if (listpack_bytes <= Listpack::kListpackHeaderSize + 1) {
    return std::nullopt;
  }
  return Prev(listpack_bytes - 1);
}

std::optional<size_t> Listpack::Next(size_t offset) const {
  RS_CHECK(offset >= kListpackHeaderSize && offset < TotalBytes());
  if (listpack_[offset] == kListpackEof) {
    return std::nullopt;
  }
  size_t next_offset = Skip(offset);
  return listpack_[next_offset] != kListpackEof
             ? std::optional<size_t>(next_offset)
             : std::nullopt;
}

std::optional<size_t> Listpack::Prev(size_t offset) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListpackHeaderSize && offset < listpack_bytes);
  if (listpack_bytes <= kListpackHeaderSize + 1 ||
      offset == kListpackHeaderSize) {
    return std::nullopt;
  }
  return PrevUnchecked(offset);
}

size_t Listpack::PrevUnchecked(size_t offset) const {
  // Decode the backlen starting from the last byte of the previous element's
  // backlen.
  const size_t backlen = DecodeBacklen(offset - 1);
  return offset - backlen - BacklenBytes(backlen);
}

unsigned char* Listpack::StringAt(size_t offset, size_t* const length,
                                  EncodingType encoding_type) const {
  if (length != nullptr) {
    *length = DecodeStringLength(offset);
  }
  switch (encoding_type) {
    case EncodingType::k6BitString:
      return listpack_.get() + offset + 1;
    case EncodingType::k12BitString:
      return listpack_.get() + offset + 2;
    case EncodingType::k32BitString:
      return listpack_.get() + offset + 5;
    default:
      return nullptr;
  }
}

unsigned char* Listpack::IntegerAt(size_t offset, unsigned char* destination,
                                   size_t* const length, int64_t* integer_value,
                                   EncodingType encoding_type) const {
  int64_t value = 0;
  uint64_t unsigned_value = 0;
  uint64_t negative_start = 0;
  uint64_t negative_max = 0;
  switch (encoding_type) {
    case EncodingType::k7BitUnsignedInteger:
      unsigned_value = listpack_[offset] & 0x7f;
      // Avoid sign conversion for the unsigned 7-bit format.
      negative_start = std::numeric_limits<uint64_t>::max();
      break;
    case EncodingType::k13BitInteger:
      unsigned_value =
          ((listpack_[offset] & 0x1f) << 8) | (listpack_[offset + 1] & 0xff);
      negative_start = 1 << 12;
      negative_max = std::numeric_limits<uint16_t>::max() >> 3;
      break;
    case EncodingType::k16BitInteger:
      unsigned_value = LoadLittleEndian<2>(&listpack_[offset + 1]);
      negative_start = 1 << 15;
      negative_max = std::numeric_limits<uint16_t>::max();
      break;
    case EncodingType::k24BitInteger:
      unsigned_value = LoadLittleEndian<3>(&listpack_[offset + 1]);
      negative_start = 1L << 23;
      negative_max = std::numeric_limits<uint32_t>::max() >> 8;
      break;
    case EncodingType::k32BitInteger:
      unsigned_value = LoadLittleEndian<4>(&listpack_[offset + 1]);
      negative_start = 1UL << 31;
      negative_max = std::numeric_limits<uint32_t>::max();
      break;
    case EncodingType::k64BitInteger:
      unsigned_value = LoadLittleEndian<8>(&listpack_[offset + 1]);
      negative_start = 1UL << 63;
      negative_max = std::numeric_limits<uint64_t>::max();
      break;
    default:
      return nullptr;
  }
  if (unsigned_value >= negative_start) {
    unsigned_value = negative_max - unsigned_value;
    value = static_cast<int64_t>(unsigned_value);
    value = -value - 1;
  } else {
    value = static_cast<int64_t>(unsigned_value);
  }
  if (destination != nullptr) {
    const int string_length = utils::Int64ToString(
        {reinterpret_cast<char*>(destination), kListpackIntBufferSize}, value);
    if (length != nullptr) {
      *length = string_length;
    }
    return destination;
  }
  if (integer_value != nullptr) {
    *integer_value = value;
  }

  return nullptr;
}

bool Listpack::Insert(size_t offset, Listpack::Position where,
                      const std::string_view* element_string,
                      const int64_t* element_integer) {
  if ((element_string == nullptr) && (element_integer == nullptr)) {
    return false;
  }
  uint32_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListpackHeaderSize && offset < listpack_bytes);
  if (where == Position::kInsertAfter) {
    offset = Skip(offset);
  }
  constexpr size_t kMaxStringEncodingOverhead = 10;
  if (element_string != nullptr &&
      element_string->size() >
          std::numeric_limits<uint32_t>::max() - kMaxStringEncodingOverhead) {
    return false;
  }
  size_t backlen = 0;
  EncodingGeneralType encoding_type = EncodingGeneralType::kInteger;
  int64_t integer_value = (element_integer != nullptr) ? *element_integer : 0;
  if ((element_integer != nullptr) ||
      utils::ToInt64(*element_string, &integer_value)) {
    backlen = EncodeInteger(nullptr, integer_value);
    encoding_type = EncodingGeneralType::kInteger;
  } else {
    backlen = EncodeString(nullptr, *element_string);
    encoding_type = EncodingGeneralType::kString;
  }
  uint8_t backlen_bytes = BacklenBytes(backlen);
  size_t replaced_bytes = 0;
  if (where == Position::kReplace) {
    size_t cur_backlen = Backlen(offset);
    size_t cur_backlen_bytes = BacklenBytes(cur_backlen);
    replaced_bytes = cur_backlen + cur_backlen_bytes;
  }
  const size_t retained_bytes = listpack_bytes - replaced_bytes;
  const size_t encoded_bytes = backlen + backlen_bytes;
  if (encoded_bytes > std::numeric_limits<uint32_t>::max() - retained_bytes) {
    return false;
  }
  const size_t new_listpack_bytes = retained_bytes + encoded_bytes;
  // Grow before memmove so the destination range is valid.
  if (new_listpack_bytes > listpack_bytes) {
    Realloc(new_listpack_bytes);
  }
  std::memmove(listpack_.get() + offset + backlen + backlen_bytes,
               listpack_.get() + offset + replaced_bytes,
               listpack_bytes - offset - replaced_bytes);
  if (where == Position::kInsertBefore) {
    uint16_t element_count = ElementCount();
    if (element_count != kListpackUnknownElementCount) {
      SetNumOfElements(element_count + 1);
    }
  }
  // Shrink after memmove so the source range remains valid.
  if (new_listpack_bytes < listpack_bytes) {
    Realloc(new_listpack_bytes);
  }
  SetTotalBytes(new_listpack_bytes);
  if (encoding_type == EncodingGeneralType::kInteger) {
    EncodeInteger(listpack_.get() + offset, integer_value);
  } else {
    EncodeString(listpack_.get() + offset, *element_string);
  }
  return true;
}

bool Listpack::PlanEntry(const ListpackEntry& entry, Encoding* const encoding) {
  constexpr size_t kMaxStringEncodingOverhead = 10;
  if (!entry.is_integer &&
      entry.string_value.size() >
          std::numeric_limits<uint32_t>::max() - kMaxStringEncodingOverhead) {
    return false;
  }
  int64_t integer_value = entry.integer_value;
  if (entry.is_integer || utils::ToInt64(entry.string_value, &integer_value)) {
    encoding->integer_value = integer_value;
    encoding->encoding_type = EncodingGeneralType::kInteger;
    encoding->backlen_bytes =
        BacklenBytes(EncodeInteger(nullptr, integer_value));
  } else {
    encoding->string_value = entry.string_value;
    encoding->integer_value = 0;
    encoding->encoding_type = EncodingGeneralType::kString;
    encoding->backlen_bytes =
        BacklenBytes(EncodeString(nullptr, entry.string_value));
  }
  return true;
}

size_t Listpack::EncodedEntryBytes(const Encoding& encoding) {
  const size_t backlen = encoding.encoding_type == EncodingGeneralType::kInteger
                             ? EncodeInteger(nullptr, encoding.integer_value)
                             : EncodeString(nullptr, encoding.string_value);
  return backlen + encoding.backlen_bytes;
}

bool Listpack::BatchInsert(size_t offset, Listpack::Position where,
                           const ListpackEntry* const entries,
                           size_t entry_count) {
  if (entry_count == 0) {
    return false;
  }
  uint32_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListpackHeaderSize && offset < listpack_bytes);
  if (where == Position::kInsertAfter) {
    offset = Skip(offset);
  }
  // Size every entry first so insertion needs one Realloc and one memmove;
  // the second pass below re-plans each entry instead of storing plans.
  size_t inserted_bytes = 0;
  for (size_t i = 0; i < entry_count; ++i) {
    Encoding encoding{};
    if (!PlanEntry(entries[i], &encoding)) {
      return false;
    }
    const size_t entry_bytes = EncodedEntryBytes(encoding);
    if (entry_bytes > std::numeric_limits<size_t>::max() - inserted_bytes) {
      return false;
    }
    inserted_bytes += entry_bytes;
  }
  if (inserted_bytes > std::numeric_limits<uint32_t>::max() - listpack_bytes) {
    return false;
  }
  const size_t new_listpack_bytes = listpack_bytes + inserted_bytes;
  // Total bytes is a 4 byte unsigned integer, so the maximum bytes for the
  // listpack is UINT32_MAX.
  if (new_listpack_bytes > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  if (new_listpack_bytes < kListpackHeaderSize + 1) {
    return false;
  }
  uint16_t element_count = ElementCount();
  Realloc(new_listpack_bytes);
  std::memmove(listpack_.get() + offset + inserted_bytes,
               listpack_.get() + offset, listpack_bytes - offset);
  // Update number of elements and total bytes.
  if (element_count != kListpackUnknownElementCount) {
    if (std::cmp_greater(entry_count,
                         kListpackUnknownElementCount - element_count)) {
      SetNumOfElements(kListpackUnknownElementCount);
    } else {
      SetNumOfElements(element_count + entry_count);
    }
  }
  SetTotalBytes(new_listpack_bytes);
  // Insert elements based on encoding types.
  for (size_t i = 0; i < entry_count; ++i) {
    Encoding encoding{};
    PlanEntry(entries[i], &encoding);
    if (encoding.encoding_type == EncodingGeneralType::kInteger) {
      offset += EncodeInteger(listpack_.get() + offset, encoding.integer_value);
    } else {
      offset += EncodeString(listpack_.get() + offset, encoding.string_value);
    }
    offset += encoding.backlen_bytes;
  }
  return true;
}

uint32_t Listpack::TotalBytes() const {
  return static_cast<uint32_t>(LoadLittleEndian<4>(listpack_.get()));
}

void Listpack::SetTotalBytes(uint32_t listpack_bytes) {
  StoreLittleEndian<4>(listpack_bytes, listpack_.get());
}

size_t Listpack::Size() const {
  uint16_t element_count = ElementCount();
  if (element_count != kListpackUnknownElementCount) {
    return element_count;
  }
  // Too many elements in the listpack, need to scan the entire listpack to get
  // the total number.
  size_t count = 0;
  std::optional<size_t> offset = First();
  while (offset.has_value()) {
    offset = Next(*offset);
    ++count;
  }
  if (count < kListpackUnknownElementCount) {
    SetNumOfElements(count);
  }
  return count;
}

uint16_t Listpack::ElementCount() const {
  return static_cast<uint16_t>(LoadLittleEndian<2>(listpack_.get() + 4));
}

void Listpack::SetNumOfElements(uint16_t element_count) const {
  StoreLittleEndian<2>(element_count, listpack_.get() + 4);
}

// Return the beginning offset of the next element of the current element at the
// given offset. If the offset is pointing to the EOF, return the current
// offset.
size_t Listpack::Skip(size_t offset) const {
  if (listpack_[offset] == kListpackEof) {
    return offset;
  }
  size_t backlen = Backlen(offset);
  uint8_t backlen_bytes = BacklenBytes(backlen);
  size_t next_offset = offset + backlen + backlen_bytes;
  return next_offset;
}

// Estimate the bytes needed to store the same integer multiple times.
size_t Listpack::EstimateBytes(int64_t value, size_t repeat) {
  const size_t encoded = EncodeInteger(nullptr, value);
  const size_t entry_bytes = encoded + BacklenBytes(encoded);
  if (repeat > (std::numeric_limits<size_t>::max() - kListpackHeaderSize - 1) /
                   entry_bytes) {
    return std::numeric_limits<size_t>::max();
  }
  return kListpackHeaderSize + (entry_bytes * repeat) + 1;
}

size_t Listpack::EstimateEntryBytes(std::string_view value) {
  int64_t integer = 0;
  constexpr size_t kMaxStringEncodingOverhead = 10;
  const bool is_integer = utils::ToInt64(value, &integer);
  if (!is_integer && value.size() > std::numeric_limits<uint32_t>::max() -
                                        kMaxStringEncodingOverhead) {
    return std::numeric_limits<size_t>::max();
  }
  const size_t encoded = is_integer ? EncodeInteger(nullptr, integer)
                                    : EncodeString(nullptr, value);
  if (encoded > std::numeric_limits<size_t>::max() - BacklenBytes(encoded)) {
    return std::numeric_limits<size_t>::max();
  }
  return encoded + BacklenBytes(encoded);
}

// Return whether adding bytes keeps the listpack within the safe size limit.
bool Listpack::SafeToAdd(const Listpack* const listpack, size_t bytes) {
  const size_t length = (listpack != nullptr) ? listpack->TotalBytes() : 0;
  return length <= kListpackMaxSafetySize &&
         bytes <= kListpackMaxSafetySize - length;
}

// Get the encoding type of the element at the given offset of the listpack.
// The function assumes that the offset is valid.
Listpack::EncodingType Listpack::EncodingAt(size_t offset) const {
  const unsigned char* buffer = listpack_.get() + offset;
  if ((buffer[0] & EncodingTypeMask::kType7BitUintMask) ==
      EncodingType::k7BitUnsignedInteger) {
    return EncodingType::k7BitUnsignedInteger;
  }
  if ((buffer[0] & EncodingTypeMask::kType6BitStrMask) ==
      EncodingType::k6BitString) {
    return EncodingType::k6BitString;
  }
  if ((buffer[0] & EncodingTypeMask::kType13BitIntMask) ==
      EncodingType::k13BitInteger) {
    return EncodingType::k13BitInteger;
  }
  if ((buffer[0] & EncodingTypeMask::kType12BitStrMask) ==
      EncodingType::k12BitString) {
    return EncodingType::k12BitString;
  }
  switch (buffer[0]) {
    case EncodingType::k16BitInteger:
      return EncodingType::k16BitInteger;
    case EncodingType::k24BitInteger:
      return EncodingType::k24BitInteger;
    case EncodingType::k32BitInteger:
      return EncodingType::k32BitInteger;
    case EncodingType::k64BitInteger:
      return EncodingType::k64BitInteger;
    default:
      return EncodingType::k32BitString;
  }
}

// Get back length from the given element of the listpack. Note that the offset
// provided is the beginning of the element.
size_t Listpack::Backlen(size_t offset) const {
  return EncodedBytes(offset, EncodingAt(offset));
}

size_t Listpack::EncodedBytes(size_t offset, EncodingType encoding_type) const {
  switch (encoding_type) {
    case EncodingType::k7BitUnsignedInteger:
      return 1;
    case EncodingType::k6BitString:
      return 1 + DecodeStringLength(offset);
    case EncodingType::k13BitInteger:
      return 2;
    case EncodingType::k12BitString:
      return 2 + DecodeStringLength(offset);
    case EncodingType::k16BitInteger:
      return 3;
    case EncodingType::k24BitInteger:
      return 4;
    case EncodingType::k32BitInteger:
      return 5;
    case EncodingType::k32BitString:
      return 5 + DecodeStringLength(offset);
    default:
      return 9;
  }
}

size_t Listpack::Seek(size_t position, size_t size) const {
  RS_CHECK(position < size);
  if (position < size - position) {
    size_t offset = kListpackHeaderSize;
    for (size_t index = 0; index < position; ++index) {
      offset = Skip(offset);
    }
    return offset;
  }
  // Closer to the tail: step back from the last element over backlens.
  size_t offset = TotalBytes() - 1;
  for (size_t index = size; index > position; --index) {
    offset = PrevUnchecked(offset);
  }
  return offset;
}

Listpack::DecodedEntry Listpack::DecodeEntry(
    size_t offset, unsigned char* const int_buffer) const {
  const unsigned char* const buffer = listpack_.get() + offset;
  // Short strings are the common case: read their length from the encoding
  // byte once instead of classifying the encoding for each field. Their
  // encoded size is at most 64 bytes, so the backlen is one byte.
  if ((buffer[0] & EncodingTypeMask::kType6BitStrMask) ==
      EncodingType::k6BitString) {
    const size_t length = buffer[0] & 0x3f;
    static_assert(1 + 0x3f <= BacklenThreshold::kSize1ByteBacklenMax);
    return {buffer + 1, length, offset + 1 + length + 1};
  }
  if ((buffer[0] & EncodingTypeMask::kType12BitStrMask) ==
      EncodingType::k12BitString) {
    const size_t length =
        (static_cast<size_t>(buffer[0] & 0xf) << 8) | buffer[1];
    const size_t encoded_bytes = 2 + length;
    return {buffer + 2, length,
            offset + encoded_bytes + BacklenBytes(encoded_bytes)};
  }
  const EncodingType encoding_type = EncodingAt(offset);
  DecodedEntry entry{};
  entry.data = IsString(encoding_type)
                   ? StringAt(offset, &entry.length, encoding_type)
                   : IntegerAt(offset, int_buffer, &entry.length, nullptr,
                               encoding_type);
  const size_t encoded_bytes = EncodedBytes(offset, encoding_type);
  entry.next = offset + encoded_bytes + BacklenBytes(encoded_bytes);
  return entry;
}

// Get number of bytes required to encode the back length.
uint8_t Listpack::BacklenBytes(size_t backlen) {
  if (backlen <= BacklenThreshold::kSize1ByteBacklenMax) {
    return 1;
  }
  if (backlen > BacklenThreshold::kSize1ByteBacklenMax &&
      backlen <= BacklenThreshold::kSize2BytesBacklenMax) {
    return 2;
  }
  if (backlen > BacklenThreshold::kSize2BytesBacklenMax &&
      backlen <= BacklenThreshold::kSize3BytesBacklenMax) {
    return 3;
  }
  if (backlen > BacklenThreshold::kSize3BytesBacklenMax &&
      backlen <= BacklenThreshold::kSize4BytesBacklenMax) {
    return 4;
  }
  return 5;
}

// Encode the string into the given pointer of the listpack and return the
// number of bytes needed to encode the element (not include bytes used to
// encode back length).
size_t Listpack::EncodeString(unsigned char* const buffer,
                              std::string_view element) {
  const auto* element_string =
      reinterpret_cast<const unsigned char*>(element.data());
  const size_t length = element.size();
  size_t backlen = 0;
  if (length <= 63) {
    // 6 bit length string
    backlen = 1 + length;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k6BitString | length;
      std::memcpy(buffer + 1, element_string, length);
    }
  } else if (length <= 4095) {
    // 12 bit length string
    backlen = 2 + length;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k12BitString | (length >> 8);
      buffer[1] = length & 0xff;
      std::memcpy(buffer + 2, element_string, length);
    }
  } else {
    // 32 bit length string
    backlen = 5 + length;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k32BitString;
      StoreLittleEndian<4>(length, buffer + 1);
      std::memcpy(buffer + 5, element_string, length);
    }
  }
  if (buffer != nullptr) {
    EncodeBacklen(buffer + backlen, backlen);
  }
  return backlen;
}

// Encode the int64 into the given pointer of the listpack and return the number
// of bytes needed to encode the element (not include bytes used to encode back
// length).
size_t Listpack::EncodeInteger(unsigned char* const buffer, int64_t value) {
  size_t backlen = 0;
  if (value >= 0 && value <= kUint7BitIntMax) {
    // 7 bit unsigned integer
    backlen = 1;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k7BitUnsignedInteger | value;
    }
  } else if (value >= -4096 && value <= 4095) {
    // 13 bit integer
    if (value < 0) {
      value += (1 << 13);
    }
    backlen = 2;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k13BitInteger | (value >> 8);
      buffer[1] = value & 0xff;
    }
  } else if (value >= std::numeric_limits<int16_t>::min() &&
             value <= std::numeric_limits<int16_t>::max()) {
    // 16 bit integer
    if (value < 0) {
      value += (1 << 16);
    }
    backlen = 3;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k16BitInteger;
      StoreLittleEndian<2>(static_cast<uint64_t>(value), buffer + 1);
    }
  } else if (value >= kInt24BitIntMin && value <= kInt24BitIntMax) {
    // 24 bit integer
    if (value < 0) {
      value += (1 << 24);
    }
    backlen = 4;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k24BitInteger;
      StoreLittleEndian<3>(static_cast<uint64_t>(value), buffer + 1);
    }
  } else if (value >= std::numeric_limits<int32_t>::min() &&
             value <= std::numeric_limits<int32_t>::max()) {
    // 32 bit integer
    if (value < 0) {
      value += (static_cast<int64_t>(1) << 32);
    }
    backlen = 5;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k32BitInteger;
      StoreLittleEndian<4>(static_cast<uint64_t>(value), buffer + 1);
    }
  } else {
    // 64 bit integer
    uint64_t unsigned_value = value;
    backlen = 9;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k64BitInteger;
      StoreLittleEndian<8>(unsigned_value, buffer + 1);
    }
  }
  if (buffer != nullptr) {
    EncodeBacklen(buffer + backlen, backlen);
  }
  return backlen;
}

// Encode the back length to the given pointer of the listpack.
void Listpack::EncodeBacklen(unsigned char* const buffer, size_t backlen) {
  // Redis' lpEncodeBacklen: the most significant 7 bits come first, and
  // every later byte has its high bit set, so a backward read from the
  // entry's end takes the low bits first and stops at the first byte.
  const uint8_t backlen_bytes = BacklenBytes(backlen);
  for (uint8_t i = 0; i < backlen_bytes; ++i) {
    const unsigned shift = 7U * (backlen_bytes - 1U - i);
    buffer[i] = static_cast<unsigned char>((backlen >> shift) & 127);
    if (i > 0) {
      buffer[i] |= 128;
    }
  }
}

// Decode the backlen of the element given its last byte through traversing
// reversely to find the first byte has its most siginificant bit set to zero.
// The function assumes that the offset is valid and is the last byte of the
// element.
size_t Listpack::DecodeBacklen(size_t offset) const {
  // Redis' lpDecodeBacklen: read backward from the entry's last byte, low
  // bits first, until a byte without the high bit.
  size_t backlen = 0;
  unsigned shift = 0;
  while (true) {
    const unsigned char byte = listpack_[offset];
    backlen |= static_cast<size_t>(byte & 127) << shift;
    if ((byte & 128) == 0) {
      break;
    }
    shift += 7;
    --offset;
  }
  return backlen;
}

// Decode the string length from the given element of the listpack.
// The function assumes that the offset is valid and the element at the offset
// is of string type.
size_t Listpack::DecodeStringLength(size_t offset) const {
  const unsigned char* buffer = listpack_.get() + offset;
  switch (EncodingAt(offset)) {
    case EncodingType::k6BitString:
      return buffer[0] & 0x3f;
    case EncodingType::k12BitString:
      return ((buffer[0] & 0xf) << 8) | buffer[1];
    default:
      return LoadLittleEndian<4>(buffer + 1);
  }
}

bool Listpack::IsString(EncodingType encoding_type) {
  return encoding_type == EncodingType::k6BitString ||
         encoding_type == EncodingType::k12BitString ||
         encoding_type == EncodingType::k32BitString;
}

void Listpack::Realloc(size_t bytes) {
  RS_CHECK(bytes >= kListpackHeaderSize + 1);
  // Keep spare capacity so repeated inserts do not copy the whole listpack
  // each time, but release memory once most of it is unused.
  if (bytes <= capacity_ && bytes >= capacity_ / 4) {
    return;
  }
  size_t new_capacity = bytes;
  if (bytes > capacity_) {
    new_capacity = std::max(bytes, capacity_ + (capacity_ / 2));
  }
  // Every byte up to TotalBytes() is copied, and the rest is written before
  // it is read, so the new storage needs no zero-fill.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): packed byte storage
  std::unique_ptr<unsigned char[]> new_listpack(
      new unsigned char[new_capacity]);
  const size_t copy_bytes =
      std::min(static_cast<size_t>(TotalBytes()), new_capacity);
  std::memcpy(new_listpack.get(), listpack_.get(), copy_bytes);
  listpack_ = std::move(new_listpack);
  capacity_ = new_capacity;
}
}  // namespace redis_simple::containers
