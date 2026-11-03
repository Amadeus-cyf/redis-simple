#ifndef REDIS_SIMPLE_MEMORY_LISTPACK_H_
#define REDIS_SIMPLE_MEMORY_LISTPACK_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::in_memory {
// Compact Redis-style sequential encoding. See:
// https://github.com/antirez/listpack/blob/master/listpack.md
class ListPack {
 public:
  struct ListPackEntry {
    std::string_view str;
    int64_t sval;
    bool is_integer;
  };
  // Header: 32-bit total bytes followed by 16-bit element count.
  static constexpr int kListPackHeaderSize = 6;
  ListPack();
  ListPack(const ListPack&) = delete;
  ListPack& operator=(const ListPack&) = delete;
  unsigned char* Get(size_t idx, size_t* len) const;
  std::optional<std::string> Get(size_t idx) const;
  std::optional<int64_t> IntegerAt(size_t idx) const;
  std::optional<size_t> Find(std::string_view val) const;
  std::optional<size_t> FindAndSkip(std::string_view val, size_t skip) const;
  bool Append(std::string_view element_string);
  bool Append(int64_t element_integer);
  bool Prepend(std::string_view element_string);
  bool Prepend(int64_t element_integer);
  bool Insert(size_t idx, std::string_view element_string);
  bool Insert(size_t idx, int64_t element_integer);
  bool Replace(size_t idx, std::string_view element_string);
  bool Replace(size_t idx, int64_t element_integer);
  bool BatchAppend(const std::vector<ListPackEntry>& entries);
  bool BatchAppend(std::initializer_list<ListPackEntry> entries);
  bool BatchPrepend(const std::vector<ListPackEntry>& entries);
  bool BatchInsert(size_t idx, const std::vector<ListPackEntry>& entries);
  std::optional<size_t> IndexAt(size_t index) const;
  // The value view is valid only during the callback invocation.
  template <typename Visitor>
  bool ForEach(size_t start, size_t stop, Visitor&& visitor) const;
  template <typename Visitor>
  bool ForEachReverse(size_t start, size_t stop, Visitor&& visitor) const;
  // The pair views are valid only during the callback invocation.
  template <typename Visitor>
  bool ForEachPair(Visitor&& visitor) const;
  void Delete(size_t idx);
  size_t DeleteRange(size_t idx, size_t count);
  size_t DeleteMatching(std::string_view value, size_t limit,
                        bool from_tail = false);
  std::optional<size_t> First() const;
  std::optional<size_t> Last() const;
  std::optional<size_t> Next(size_t idx) const;
  std::optional<size_t> Prev(size_t idx) const;
  uint32_t TotalBytes() const;
  size_t Size() const;
  static size_t EstimateEntryBytes(std::string_view value);
  static size_t EstimateBytes(int64_t lval, size_t repeat);
  static bool SafeToAdd(const ListPack* lp, size_t bytes);
  ~ListPack() = default;

 private:
  // Enough space for INT64_MIN plus a null terminator.
  static constexpr int kListPackIntBufSize = 21;
  static constexpr uint16_t kListPackNumEleUnknown = UINT16_MAX;
  static constexpr int kUint7BitIntMax = 127;
  static constexpr int kInt24BitIntMax = (1 << 23) - 1;
  static constexpr int kInt24BitIntMin = -(1 << 23);
  // Keep reallocations below the unsigned 32-bit total-bytes limit.
  static constexpr uint32_t kListPackMaxSafetySize = 1 << 30;
  static constexpr int64_t kListPackEof = 0xff;
  enum class EncodingGeneralType {
    kInteger = 0,
    kString = 1,
  };
  enum EncodingType {
    k7BitUnsignedInteger = 0,
    k6BitString = 0x80,
    k13BitInteger = 0xc0,
    k12BitString = 0xe0,
    k32BitString = 0xf0,
    k16BitInteger = 0xf1,
    k24BitInteger = 0xf2,
    k32BitInteger = 0xf3,
    k64BitInteger = 0xf4,
  };
  enum EncodingTypeMask {
    kType7BitUintMask = 0x80,
    kType6BitStrMask = 0xc0,
    kType13BitIntMask = 0xe0,
    kType12BitStrMask = 0xf0,
    kType16BitIntMask = 0xff,
    kType24BitIntMask = 0xff,
    kType32BitIntMask = 0xff,
    kType32BitStrMask = 0xff,
    kType64BitIntMask = 0xff,
  };
  enum class Position {
    kInsertAfter = 0,
    kInsertBefore = 1,
    kReplace = 2,
  };
  enum BacklenThreshold : uint64_t {
    kSize1ByteBacklenMax = (1ULL << 7) - 1,
    kSize2BytesBacklenMax = (1ULL << 14) - 1,
    kSize3BytesBacklenMax = (1ULL << 21) - 1,
    kSize4BytesBacklenMax = (1ULL << 28) - 1,
    kSize5BytesBacklenMax = (1ULL << 35) - 1,
  };
  struct Encoding {
    std::string_view str;
    int64_t sval;
    EncodingGeneralType encoding_type;
    size_t backlen_bytes;
  };
  unsigned char* StringAt(size_t idx, size_t* len,
                          EncodingType encoding_type) const;
  unsigned char* IntegerAt(size_t idx, unsigned char* dst, size_t* len,
                           int64_t* val, EncodingType encoding_type) const;
  bool Insert(size_t idx, ListPack::Position where,
              const std::string_view* element_string,
              const int64_t* element_integer);
  bool BatchInsert(size_t idx, ListPack::Position where,
                   const ListPackEntry* entries, size_t entry_count);
  // Chooses the encoding for `entry`; returns false if it is too large.
  static bool PlanEntry(const ListPackEntry& entry, Encoding* encoding);
  static size_t EncodedEntryBytes(const Encoding& encoding);
  void SetTotalBytes(uint32_t listpack_bytes);
  uint16_t ElementCount() const;
  void SetNumOfElements(uint16_t num_of_elements) const;
  size_t Skip(size_t idx) const;
  EncodingType EncodingAt(size_t idx) const;
  size_t Backlen(size_t idx) const;
  // Bytes of the element at `idx` before its backlen, given its encoding.
  size_t EncodedBytes(size_t idx, EncodingType encoding_type) const;
  // An element's value bytes and the offset just past it, which is the EOF
  // byte for the last element.
  struct DecodedEntry {
    const unsigned char* data;
    size_t length;
    size_t next;
  };
  // Decodes the element at `idx`, which must not be EOF, reading its encoding
  // once for both the value and the next offset. Integers are rendered into
  // `int_buf`, which must hold kListPackIntBufSize bytes. Iterators use this
  // instead of Get() followed by Next(), which decode each element twice.
  DecodedEntry DecodeEntry(size_t idx, unsigned char* int_buf) const;
  // Returns the offset of element `position`, which must be below `size`,
  // the element count. Walks from whichever end is closer.
  size_t Seek(size_t position, size_t size) const;
  // Offset of the element before the one at `idx`, which may be the EOF
  // byte; `idx` must not be the first element.
  size_t PrevUnchecked(size_t idx) const;
  static std::string_view ValueView(const DecodedEntry& entry) {
    return {reinterpret_cast<const char*>(entry.data), entry.length};
  }
  static uint8_t BacklenBytes(size_t backlen);
  static size_t EncodeString(unsigned char* buf, std::string_view ele);
  static size_t EncodeInteger(unsigned char* buf, int64_t v);
  static void EncodeBacklen(unsigned char* buf, size_t backlen);
  size_t DecodeBacklen(size_t idx) const;
  size_t DecodeStringLength(size_t idx) const;
  static bool IsString(EncodingType encoding_type);
  void Realloc(size_t bytes);
  std::unique_ptr<unsigned char[]> lp_;
  // Allocated bytes of lp_; at least TotalBytes().
  size_t capacity_;
  mutable std::array<unsigned char, kListPackIntBufSize> int_buf_{};
};

template <typename Visitor>
bool ListPack::ForEach(size_t start, size_t stop, Visitor&& visitor) const {
  const size_t size = Size();
  if (start > stop || start >= size) {
    return true;
  }
  stop = std::min(stop, size - 1);

  size_t listpack_index = Seek(start, size);
  for (size_t index = start;
       index <= stop && lp_[listpack_index] != kListPackEof; ++index) {
    const DecodedEntry entry = DecodeEntry(listpack_index, int_buf_.data());
    if (!visitor(ValueView(entry))) {
      return false;
    }
    listpack_index = entry.next;
  }
  return true;
}

template <typename Visitor>
bool ListPack::ForEachReverse(size_t start, size_t stop,
                              Visitor&& visitor) const {
  const size_t size = Size();
  if (start > stop || start >= size) {
    return true;
  }
  stop = std::min(stop, size - 1);

  size_t index = stop;
  std::optional<size_t> listpack_index = Seek(stop, size);
  while (listpack_index.has_value() && index >= start) {
    if (!visitor(ValueView(DecodeEntry(*listpack_index, int_buf_.data())))) {
      return false;
    }
    if (index == 0) {
      break;
    }
    listpack_index = Prev(*listpack_index);
    --index;
  }
  return true;
}

template <typename Visitor>
bool ListPack::ForEachPair(Visitor&& visitor) const {
  const auto first = First();
  if (!first.has_value()) {
    return true;
  }
  std::array<unsigned char, kListPackIntBufSize> first_int_buf{};
  std::array<unsigned char, kListPackIntBufSize> second_int_buf{};
  size_t listpack_index = *first;
  while (lp_[listpack_index] != kListPackEof) {
    const DecodedEntry first_entry =
        DecodeEntry(listpack_index, first_int_buf.data());
    if (lp_[first_entry.next] == kListPackEof) {
      break;
    }
    const DecodedEntry second_entry =
        DecodeEntry(first_entry.next, second_int_buf.data());
    if (!visitor(ValueView(first_entry), ValueView(second_entry))) {
      return false;
    }
    listpack_index = second_entry.next;
  }
  return true;
}
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_LISTPACK_H_
