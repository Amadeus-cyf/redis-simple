#include "memory/listpack.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/check.h"
#include "utils/string_utils.h"

namespace redis_simple::in_memory {
ListPack::ListPack()
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): packed byte storage
    : listpack_(std::make_unique<unsigned char[]>(kListPackHeaderSize + 1)),
      capacity_(kListPackHeaderSize + 1) {
  SetTotalBytes(kListPackHeaderSize + 1);
  SetNumOfElements(0);
  listpack_[kListPackHeaderSize] = kListPackEof;
}

unsigned char* ListPack::Get(size_t offset, size_t* const length) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListPackHeaderSize && offset < listpack_bytes);
  if (offset == listpack_bytes - 1) {
    return nullptr;
  }
  EncodingType encoding_type = EncodingAt(offset);
  if (IsString(encoding_type)) {
    return StringAt(offset, length, encoding_type);
  }
  return IntegerAt(offset, int_buffer_.data(), length, nullptr, encoding_type);
}

std::optional<std::string> ListPack::Get(size_t offset) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListPackHeaderSize && offset < listpack_bytes);
  if (offset == listpack_bytes - 1) {
    return std::nullopt;
  }

  size_t length = 0;
  std::array<unsigned char, kListPackIntBufferSize> int_buffer{};
  EncodingType encoding_type = EncodingAt(offset);
  const unsigned char* buffer =
      IsString(encoding_type) ? StringAt(offset, &length, encoding_type)
                              : IntegerAt(offset, int_buffer.data(), &length,
                                          nullptr, encoding_type);
  return std::string(reinterpret_cast<const char*>(buffer), length);
}

std::optional<int64_t> ListPack::IntegerAt(size_t offset) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListPackHeaderSize && offset < listpack_bytes);
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

std::optional<size_t> ListPack::Find(std::string_view value) const {
  return FindAndSkip(value, 0);
}

std::optional<size_t> ListPack::FindAndSkip(std::string_view value,
                                            size_t skip) const {
  std::optional<size_t> offset = First();
  size_t skip_count = 0;
  while (offset.has_value()) {
    if (skip_count == 0) {
      size_t length = 0;
      const unsigned char* data = Get(*offset, &length);
      if (data != nullptr &&
          std::string_view(reinterpret_cast<const char*>(data), length) ==
              value) {
        return offset;
      }
      skip_count = skip;
    } else {
      --skip_count;
    }
    offset = Next(*offset);
  }
  return std::nullopt;
}

bool ListPack::Append(std::string_view element_string) {
  uint32_t listpack_bytes = TotalBytes();
  return Insert(listpack_bytes - 1, Position::kInsertBefore, &element_string,
                nullptr);
}

bool ListPack::Append(int64_t element_integer) {
  uint32_t listpack_bytes = TotalBytes();
  return Insert(listpack_bytes - 1, Position::kInsertBefore, nullptr,
                &element_integer);
}

bool ListPack::Prepend(std::string_view element_string) {
  return Insert(kListPackHeaderSize, Position::kInsertBefore, &element_string,
                nullptr);
}

bool ListPack::Prepend(int64_t element_integer) {
  return Insert(kListPackHeaderSize, Position::kInsertBefore, nullptr,
                &element_integer);
}

bool ListPack::Insert(size_t offset, std::string_view element_string) {
  return Insert(offset, Position::kInsertBefore, &element_string, nullptr);
}

bool ListPack::Insert(size_t offset, int64_t element_integer) {
  return Insert(offset, Position::kInsertBefore, nullptr, &element_integer);
}

bool ListPack::Replace(size_t offset, std::string_view element_string) {
  return Insert(offset, Position::kReplace, &element_string, nullptr);
}

bool ListPack::Replace(size_t offset, int64_t element_integer) {
  return Insert(offset, Position::kReplace, nullptr, &element_integer);
}

bool ListPack::BatchAppend(const std::vector<ListPackEntry>& entries) {
  return BatchInsert(TotalBytes() - 1, Position::kInsertBefore, entries.data(),
                     entries.size());
}

bool ListPack::BatchAppend(std::initializer_list<ListPackEntry> entries) {
  return BatchInsert(TotalBytes() - 1, Position::kInsertBefore, entries.begin(),
                     entries.size());
}

bool ListPack::BatchPrepend(const std::vector<ListPackEntry>& entries) {
  return BatchInsert(kListPackHeaderSize, Position::kInsertBefore,
                     entries.data(), entries.size());
}

bool ListPack::BatchInsert(size_t offset,
                           const std::vector<ListPackEntry>& entries) {
  return BatchInsert(offset, Position::kInsertBefore, entries.data(),
                     entries.size());
}

std::optional<size_t> ListPack::OffsetAt(size_t index) const {
  std::optional<size_t> offset = First();
  size_t current_index = 0;
  while (offset.has_value() && current_index < index) {
    offset = Next(*offset);
    ++current_index;
  }
  return offset;
}

void ListPack::Delete(size_t offset) { DeleteRange(offset, 1); }

size_t ListPack::DeleteRange(size_t offset, size_t count) {
  uint32_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListPackHeaderSize && offset < listpack_bytes);
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
  if (element_count != kListPackUnknownElementCount) {
    SetNumOfElements(element_count - deleted);
  }
  return deleted;
}

size_t ListPack::DeleteMatching(std::string_view value, size_t limit,
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
    std::array<unsigned char, kListPackIntBufferSize> int_buffer{};
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
    std::array<unsigned char, kListPackIntBufferSize> int_buffer{};
    size_t offset = kListPackHeaderSize;
    while (listpack_[offset] != kListPackEof &&
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
  if (element_count != kListPackUnknownElementCount) {
    SetNumOfElements(static_cast<uint16_t>(element_count - match_count));
  }
  return match_count;
}

std::optional<size_t> ListPack::First() const {
  size_t listpack_bytes = TotalBytes();
  if (listpack_bytes <= ListPack::kListPackHeaderSize + 1) {
    return std::nullopt;
  }
  return ListPack::kListPackHeaderSize;
}

std::optional<size_t> ListPack::Last() const {
  size_t listpack_bytes = TotalBytes();
  if (listpack_bytes <= ListPack::kListPackHeaderSize + 1) {
    return std::nullopt;
  }
  return Prev(listpack_bytes - 1);
}

std::optional<size_t> ListPack::Next(size_t offset) const {
  RS_CHECK(offset >= kListPackHeaderSize && offset < TotalBytes());
  if (listpack_[offset] == kListPackEof) {
    return std::nullopt;
  }
  size_t next_offset = Skip(offset);
  return listpack_[next_offset] != kListPackEof
             ? std::optional<size_t>(next_offset)
             : std::nullopt;
}

std::optional<size_t> ListPack::Prev(size_t offset) const {
  size_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListPackHeaderSize && offset < listpack_bytes);
  if (listpack_bytes <= kListPackHeaderSize + 1 ||
      offset == kListPackHeaderSize) {
    return std::nullopt;
  }
  return PrevUnchecked(offset);
}

size_t ListPack::PrevUnchecked(size_t offset) const {
  // Decode the backlen starting from the last byte of the previous element's
  // backlen.
  const size_t backlen = DecodeBacklen(offset - 1);
  return offset - backlen - BacklenBytes(backlen);
}

unsigned char* ListPack::StringAt(size_t offset, size_t* const length,
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

unsigned char* ListPack::IntegerAt(size_t offset, unsigned char* dst,
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
      unsigned_value = (listpack_[offset + 1] << 8) | (listpack_[offset + 2]);
      negative_start = 1 << 15;
      negative_max = std::numeric_limits<uint16_t>::max();
      break;
    case EncodingType::k24BitInteger:
      unsigned_value = (listpack_[offset + 1] << 16) |
                       ((listpack_[offset + 2]) << 8) | listpack_[offset + 3];
      negative_start = 1L << 23;
      negative_max = std::numeric_limits<uint32_t>::max() >> 8;
      break;
    case EncodingType::k32BitInteger:
      unsigned_value = (static_cast<uint64_t>(listpack_[offset + 1]) << 24) |
                       (static_cast<uint64_t>(listpack_[offset + 2]) << 16) |
                       (static_cast<uint64_t>(listpack_[offset + 3]) << 8) |
                       static_cast<uint64_t>(listpack_[offset + 4]);
      negative_start = 1UL << 31;
      negative_max = std::numeric_limits<uint32_t>::max();
      break;
    case EncodingType::k64BitInteger:
      unsigned_value = (static_cast<uint64_t>(listpack_[offset + 1]) << 56) |
                       (static_cast<uint64_t>(listpack_[offset + 2]) << 48) |
                       (static_cast<uint64_t>(listpack_[offset + 3]) << 40) |
                       (static_cast<uint64_t>(listpack_[offset + 4]) << 32) |
                       (static_cast<uint64_t>(listpack_[offset + 5]) << 24) |
                       (static_cast<uint64_t>(listpack_[offset + 6]) << 16) |
                       (static_cast<uint64_t>(listpack_[offset + 7]) << 8) |
                       static_cast<uint64_t>(listpack_[offset + 8]);
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
  if (dst != nullptr) {
    const int string_length = utils::Int64ToString(
        {reinterpret_cast<char*>(dst), kListPackIntBufferSize}, value);
    if (length != nullptr) {
      *length = string_length;
    }
    return dst;
  }
  if (integer_value != nullptr) {
    *integer_value = value;
  }

  return nullptr;
}

bool ListPack::Insert(size_t offset, ListPack::Position where,
                      const std::string_view* element_string,
                      const int64_t* element_integer) {
  if ((element_string == nullptr) && (element_integer == nullptr)) {
    return false;
  }
  uint32_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListPackHeaderSize && offset < listpack_bytes);
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
      utils::ToCanonicalInt64(*element_string, &integer_value)) {
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
    if (element_count != kListPackUnknownElementCount) {
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

bool ListPack::PlanEntry(const ListPackEntry& entry, Encoding* const encoding) {
  constexpr size_t kMaxStringEncodingOverhead = 10;
  if (!entry.is_integer &&
      entry.str.size() >
          std::numeric_limits<uint32_t>::max() - kMaxStringEncodingOverhead) {
    return false;
  }
  int64_t integer_value = entry.integer_value;
  if (entry.is_integer || utils::ToCanonicalInt64(entry.str, &integer_value)) {
    encoding->integer_value = integer_value;
    encoding->encoding_type = EncodingGeneralType::kInteger;
    encoding->backlen_bytes =
        BacklenBytes(EncodeInteger(nullptr, integer_value));
  } else {
    encoding->str = entry.str;
    encoding->integer_value = 0;
    encoding->encoding_type = EncodingGeneralType::kString;
    encoding->backlen_bytes = BacklenBytes(EncodeString(nullptr, entry.str));
  }
  return true;
}

size_t ListPack::EncodedEntryBytes(const Encoding& encoding) {
  const size_t backlen = encoding.encoding_type == EncodingGeneralType::kInteger
                             ? EncodeInteger(nullptr, encoding.integer_value)
                             : EncodeString(nullptr, encoding.str);
  return backlen + encoding.backlen_bytes;
}

bool ListPack::BatchInsert(size_t offset, ListPack::Position where,
                           const ListPackEntry* const entries,
                           size_t entry_count) {
  if (entry_count == 0) {
    return false;
  }
  uint32_t listpack_bytes = TotalBytes();
  RS_CHECK(offset >= kListPackHeaderSize && offset < listpack_bytes);
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
  if (new_listpack_bytes < kListPackHeaderSize + 1) {
    return false;
  }
  uint16_t element_count = ElementCount();
  Realloc(new_listpack_bytes);
  std::memmove(listpack_.get() + offset + inserted_bytes,
               listpack_.get() + offset, listpack_bytes - offset);
  // Update number of elements and total bytes.
  if (element_count != kListPackUnknownElementCount) {
    if (std::cmp_greater(entry_count,
                         kListPackUnknownElementCount - element_count)) {
      SetNumOfElements(kListPackUnknownElementCount);
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
      offset += EncodeString(listpack_.get() + offset, encoding.str);
    }
    offset += encoding.backlen_bytes;
  }
  return true;
}

uint32_t ListPack::TotalBytes() const {
  return (static_cast<uint32_t>(listpack_[0]) << 24) |
         (static_cast<uint32_t>(listpack_[1]) << 16) |
         (static_cast<uint32_t>(listpack_[2]) << 8) |
         static_cast<uint32_t>(listpack_[3]);
}

void ListPack::SetTotalBytes(uint32_t listpack_bytes) {
  listpack_[0] = (listpack_bytes >> 24) & 0xff;
  listpack_[1] = (listpack_bytes >> 16) & 0xff;
  listpack_[2] = (listpack_bytes >> 8) & 0xff;
  listpack_[3] = listpack_bytes & 0xff;
}

size_t ListPack::Size() const {
  uint16_t element_count = ElementCount();
  if (element_count != kListPackUnknownElementCount) {
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
  if (count < kListPackUnknownElementCount) {
    SetNumOfElements(count);
  }
  return count;
}

uint16_t ListPack::ElementCount() const {
  return (listpack_[4] << 8) | listpack_[5];
}

void ListPack::SetNumOfElements(uint16_t element_count) const {
  listpack_[4] = (element_count >> 8) & 0xff;
  listpack_[5] = element_count & 0xff;
}

/*
 * Return the beginning offset of the next element of the current element at the
 * given offset. If the offset is pointing to the EOF, return the current
 * offset.
 */
size_t ListPack::Skip(size_t offset) const {
  if (listpack_[offset] == kListPackEof) {
    return offset;
  }
  size_t backlen = Backlen(offset);
  uint8_t backlen_bytes = BacklenBytes(backlen);
  size_t next_offset = offset + backlen + backlen_bytes;
  return next_offset;
}

/*
 * Estimate the bytes needed to store the same integer multiple times.
 */
size_t ListPack::EstimateBytes(int64_t value, size_t repeat) {
  const size_t encoded = EncodeInteger(nullptr, value);
  const size_t entry_bytes = encoded + BacklenBytes(encoded);
  if (repeat > (std::numeric_limits<size_t>::max() - kListPackHeaderSize - 1) /
                   entry_bytes) {
    return std::numeric_limits<size_t>::max();
  }
  return kListPackHeaderSize + (entry_bytes * repeat) + 1;
}

size_t ListPack::EstimateEntryBytes(std::string_view value) {
  int64_t integer = 0;
  constexpr size_t kMaxStringEncodingOverhead = 10;
  const bool is_integer = utils::ToCanonicalInt64(value, &integer);
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

/*
 * Return whether adding bytes keeps the listpack within the safe size limit.
 */
bool ListPack::SafeToAdd(const ListPack* const listpack, size_t bytes) {
  const size_t length = (listpack != nullptr) ? listpack->TotalBytes() : 0;
  return length <= kListPackMaxSafetySize &&
         bytes <= kListPackMaxSafetySize - length;
}

/*
 * Get the encoding type of the element at the given offset of the listpack.
 * The function assumes that the offset is valid.
 */
ListPack::EncodingType ListPack::EncodingAt(size_t offset) const {
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

/*
 * Get back length from the given element of the listpack. Note that the offset
 * provided is the beginning of the element.
 */
size_t ListPack::Backlen(size_t offset) const {
  return EncodedBytes(offset, EncodingAt(offset));
}

size_t ListPack::EncodedBytes(size_t offset, EncodingType encoding_type) const {
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

size_t ListPack::Seek(size_t position, size_t size) const {
  RS_CHECK(position < size);
  if (position < size - position) {
    size_t offset = kListPackHeaderSize;
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

ListPack::DecodedEntry ListPack::DecodeEntry(
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

/*
 * Get number of bytes required to encode the back length.
 */
uint8_t ListPack::BacklenBytes(size_t backlen) {
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

/*
 * Encode the string into the given pointer of the listpack and return the
 * number of bytes needed to encode the element (not include bytes used to
 * encode back length).
 */
size_t ListPack::EncodeString(unsigned char* const buffer,
                              std::string_view element) {
  const unsigned char* element_string =
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
      buffer[1] = length >> 24;
      buffer[2] = (length >> 16) & 0xff;
      buffer[3] = (length >> 8) & 0xff;
      buffer[4] = length & 0xff;
      std::memcpy(buffer + 5, element_string, length);
    }
  }
  if (buffer != nullptr) {
    EncodeBacklen(buffer + backlen, backlen);
  }
  return backlen;
}

/*
 * Encode the int64 into the given pointer of the listpack and return the number
 * of bytes needed to encode the element (not include bytes used to encode back
 * length).
 */
size_t ListPack::EncodeInteger(unsigned char* const buffer, int64_t v) {
  size_t backlen = 0;
  if (v >= 0 && v <= kUint7BitIntMax) {
    // 7 bit unsigned integer
    backlen = 1;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k7BitUnsignedInteger | v;
    }
  } else if (v >= -4096 && v <= 4095) {
    // 13 bit integer
    if (v < 0) {
      v += (1 << 13);
    }
    backlen = 2;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k13BitInteger | (v >> 8);
      buffer[1] = v & 0xff;
    }
  } else if (v >= std::numeric_limits<int16_t>::min() &&
             v <= std::numeric_limits<int16_t>::max()) {
    // 16 bit integer
    if (v < 0) {
      v += (1 << 16);
    }
    backlen = 3;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k16BitInteger;
      buffer[1] = (v >> 8);
      buffer[2] = v & 0xff;
    }
  } else if (v >= kInt24BitIntMin && v <= kInt24BitIntMax) {
    // 24 bit integer
    if (v < 0) {
      v += (1 << 24);
    }
    backlen = 4;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k24BitInteger;
      buffer[1] = (v >> 16);
      buffer[2] = (v >> 8) & 0xff;
      buffer[3] = v & 0xff;
    }
  } else if (v >= std::numeric_limits<int32_t>::min() &&
             v <= std::numeric_limits<int32_t>::max()) {
    // 32 bit integer
    if (v < 0) {
      v += (static_cast<int64_t>(1) << 32);
    }
    backlen = 5;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k32BitInteger;
      buffer[1] = (v >> 24);
      buffer[2] = (v >> 16) & 0xff;
      buffer[3] = (v >> 8) & 0xff;
      buffer[4] = v & 0xff;
    }
  } else {
    // 64 bit integer
    uint64_t uv = v;
    backlen = 9;
    if (buffer != nullptr) {
      buffer[0] = EncodingType::k64BitInteger;
      buffer[1] = (uv >> 56);
      buffer[2] = (uv >> 48) & 0xff;
      buffer[3] = (uv >> 40) & 0xff;
      buffer[4] = (uv >> 32) & 0xff;
      buffer[5] = (uv >> 24) & 0xff;
      buffer[6] = (uv >> 16) & 0xff;
      buffer[7] = (uv >> 8) & 0xff;
      buffer[8] = uv & 0xff;
    }
  }
  if (buffer != nullptr) {
    EncodeBacklen(buffer + backlen, backlen);
  }
  return backlen;
}

/*
 * Encode the back length to the given pointer of the listpack.
 */
void ListPack::EncodeBacklen(unsigned char* const buffer, size_t backlen) {
  uint8_t backlen_bytes = BacklenBytes(backlen);
  for (uint8_t i = 0; i < backlen_bytes; ++i) {
    buffer[i] = backlen & 127;
    if (i > 0) {
      // Set the most significant bit to 1 if this is not the first byte of the
      // backlen.
      buffer[i] |= 128;
    }
    backlen >>= 7;
  }
}

/*
 * Decode the backlen of the element given its last byte through traversing
 * reversely to find the first byte has its most siginificant bit set to zero.
 * The function assumes that the offset is valid and is the last byte of the
 * element.
 */
size_t ListPack::DecodeBacklen(size_t offset) const {
  size_t backlen = 0;
  while (true) {
    const unsigned char byte = listpack_[offset];
    backlen = (backlen << 7) | (byte & 127);
    if ((byte & 128) == 0) {
      break;
    }
    --offset;
  }
  return backlen;
}

/*
 * Decode the string length from the given element of the listpack.
 * The function assumes that the offset is valid and the element at the offset
 * is of string type.
 */
size_t ListPack::DecodeStringLength(size_t offset) const {
  const unsigned char* buffer = listpack_.get() + offset;
  switch (EncodingAt(offset)) {
    case EncodingType::k6BitString:
      return buffer[0] & 0x3f;
    case EncodingType::k12BitString:
      return ((buffer[0] & 0xf) << 8) | buffer[1];
    default:
      return (buffer[1] << 24) | (buffer[2] << 16) | (buffer[3] << 8) |
             buffer[4];
  }
}

bool ListPack::IsString(EncodingType encoding_type) {
  return encoding_type == EncodingType::k6BitString ||
         encoding_type == EncodingType::k12BitString ||
         encoding_type == EncodingType::k32BitString;
}

void ListPack::Realloc(size_t bytes) {
  RS_CHECK(bytes >= kListPackHeaderSize + 1);
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
}  // namespace redis_simple::in_memory
