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
  unsigned char* Get(size_t idx, size_t* const len) const;
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
  static bool SafeToAdd(const ListPack* const lp, size_t bytes);
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
  unsigned char* StringAt(size_t idx, size_t* const len,
                          EncodingType encoding_type) const;
  unsigned char* IntegerAt(size_t idx, unsigned char* dst, size_t* const len,
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
  static uint8_t BacklenBytes(size_t backlen);
  static size_t EncodeString(unsigned char* buf, std::string_view ele);
  static size_t EncodeInteger(unsigned char* const buf, int64_t v);
  static void EncodeBacklen(unsigned char* const buf, size_t backlen);
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

  size_t index = 0;
  auto listpack_index = First();
  while (listpack_index.has_value() && index <= stop) {
    if (index >= start) {
      size_t len = 0;
      const auto* data = Get(*listpack_index, &len);
      if (data != nullptr) {
        const std::string_view value(reinterpret_cast<const char*>(data), len);
        if (!visitor(value)) {
          return false;
        }
      }
    }
    ++index;
    listpack_index = Next(*listpack_index);
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

  size_t index = 0;
  auto listpack_index = First();
  while (listpack_index.has_value() && index < stop) {
    listpack_index = Next(*listpack_index);
    ++index;
  }

  while (listpack_index.has_value() && index >= start) {
    size_t len = 0;
    const auto* data = Get(*listpack_index, &len);
    if (data != nullptr) {
      const std::string_view value(reinterpret_cast<const char*>(data), len);
      if (!visitor(value)) {
        return false;
      }
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
  auto first_idx = First();
  while (first_idx.has_value()) {
    const auto second_idx = Next(*first_idx);
    if (!second_idx.has_value()) {
      break;
    }

    size_t first_len = 0;
    size_t second_len = 0;
    std::array<unsigned char, kListPackIntBufSize> first_int_buf{};
    std::array<unsigned char, kListPackIntBufSize> second_int_buf{};
    const EncodingType first_type = EncodingAt(*first_idx);
    const EncodingType second_type = EncodingAt(*second_idx);
    const auto* first = IsString(first_type)
                            ? StringAt(*first_idx, &first_len, first_type)
                            : IntegerAt(*first_idx, first_int_buf.data(),
                                        &first_len, nullptr, first_type);
    const auto* second = IsString(second_type)
                             ? StringAt(*second_idx, &second_len, second_type)
                             : IntegerAt(*second_idx, second_int_buf.data(),
                                         &second_len, nullptr, second_type);
    if (first != nullptr && second != nullptr) {
      const std::string_view first_value(reinterpret_cast<const char*>(first),
                                         first_len);
      const std::string_view second_value(reinterpret_cast<const char*>(second),
                                          second_len);
      if (!visitor(first_value, second_value)) {
        return false;
      }
    }
    first_idx = Next(*second_idx);
  }
  return true;
}
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_LISTPACK_H_
