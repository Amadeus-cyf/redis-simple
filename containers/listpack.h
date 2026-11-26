#ifndef REDIS_SIMPLE_CONTAINERS_LISTPACK_H_
#define REDIS_SIMPLE_CONTAINERS_LISTPACK_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redis_simple::containers {
// Compact Redis-style sequential encoding. See:
// https://github.com/antirez/listpack/blob/master/listpack.md
// Elements are addressed by their byte offset in the buffer; any change to
// the listpack invalidates offsets. Text that is a canonical integer is
// stored as an integer.
class Listpack {
 public:
  struct ListpackEntry {
    std::string_view string_value;
    int64_t integer_value;
    bool is_integer;
  };
  // Header: 32-bit total bytes followed by 16-bit element count.
  static constexpr int kListpackHeaderSize = 6;
  // Enough space for INT64_MIN plus a null terminator.
  static constexpr int kListpackIntBufferSize = 21;
  // Where ElementAt formats an integer element as text.
  using IntBuffer = std::array<unsigned char, kListpackIntBufferSize>;
  Listpack();
  // A listpack holding a copy of `bytes`, which must be a whole listpack in
  // Redis' byte format whose elements encode as this class encodes them:
  // validate untrusted bytes first, as the RDB loader does with
  // ListpackReader. Checks only the header and terminator.
  static std::unique_ptr<Listpack> CreateFromBytes(std::string_view bytes);
  Listpack(const Listpack&) = delete;
  Listpack& operator=(const Listpack&) = delete;
  // A moved-from Listpack is unallocated (see Unallocated).
  Listpack(Listpack&&) noexcept = default;
  Listpack& operator=(Listpack&&) noexcept = default;
  // A Listpack that owns no bytes, for a container that holds its elements
  // in another encoding: it costs no allocation and no flag beside it.
  // Only Allocated, Release, assignment and destruction are valid on one.
  static Listpack Unallocated() { return Listpack(nullptr, 0); }
  bool Allocated() const { return listpack_ != nullptr; }
  // Frees the bytes, leaving the Listpack unallocated.
  void Release() {
    listpack_.reset();
    capacity_ = 0;
  }
  // The element at `offset` as text, or nullopt at the terminator. An
  // integer element is formatted into `buffer`; the view is valid while the
  // listpack is unchanged and, for an integer, while `buffer` holds it.
  std::optional<std::string_view> ElementAt(size_t offset,
                                            IntBuffer* buffer) const;
  std::optional<std::string> Get(size_t offset) const;
  std::optional<int64_t> IntegerAt(size_t offset) const;
  // The offset of the first element equal to `value`. FindAndSkip passes
  // over `skip` elements after each one it compares, so a skip of 1 looks
  // at every other element (the fields of field-value pairs).
  std::optional<size_t> Find(std::string_view value) const;
  std::optional<size_t> FindAndSkip(std::string_view value, size_t skip) const;
  // Insert places the new element before the one at `offset`. The adding
  // and replacing functions return false, changing nothing, if the listpack
  // would grow too large.
  bool Append(std::string_view element_string);
  bool Append(int64_t element_integer);
  bool Prepend(std::string_view element_string);
  bool Prepend(int64_t element_integer);
  bool Insert(size_t offset, std::string_view element_string);
  bool Insert(size_t offset, int64_t element_integer);
  bool Replace(size_t offset, std::string_view element_string);
  bool Replace(size_t offset, int64_t element_integer);
  bool BatchAppend(const std::vector<ListpackEntry>& entries);
  bool BatchAppend(std::initializer_list<ListpackEntry> entries);
  bool BatchPrepend(const std::vector<ListpackEntry>& entries);
  bool BatchInsert(size_t offset, const std::vector<ListpackEntry>& entries);
  bool BatchInsert(size_t offset, std::initializer_list<ListpackEntry> entries);
  // The offset of the element at `index`, or nullopt past the end.
  std::optional<size_t> OffsetAt(size_t index) const;
  // Call `visitor(std::string_view)` for the elements at indexes `start`
  // to `stop`, both included; ForEachReverse goes from `stop` down. A
  // visitor that returns false stops the walk, which then returns false.
  // The value view is valid only during the callback invocation.
  template <typename Visitor>
  bool ForEach(size_t start, size_t stop, Visitor&& visitor) const;
  template <typename Visitor>
  bool ForEachReverse(size_t start, size_t stop, Visitor&& visitor) const;
  // The pair views are valid only during the callback invocation.
  template <typename Visitor>
  bool ForEachPair(Visitor&& visitor) const;
  void Delete(size_t offset);
  // Delete up to `count` elements starting with the one at `offset`, and
  // return how many were deleted.
  size_t DeleteRange(size_t offset, size_t count);
  // Delete elements equal to `value`, at most `limit` of them (0 for all),
  // taken from the tail first if `from_tail`. Returns how many were deleted.
  size_t DeleteMatching(std::string_view value, size_t limit,
                        bool from_tail = false);
  std::optional<size_t> First() const;
  std::optional<size_t> Last() const;
  std::optional<size_t> Next(size_t offset) const;
  std::optional<size_t> Prev(size_t offset) const;
  uint32_t TotalBytes() const;
  // The encoded listpack, in Redis' byte format, as RDB files store it.
  std::string_view Bytes() const {
    return {reinterpret_cast<const char*>(listpack_.get()), TotalBytes()};
  }
  // The number of elements. Walks the whole listpack when there are too
  // many for the header's 16-bit count.
  size_t Size() const;
  static size_t EstimateEntryBytes(std::string_view value);
  static size_t EstimateBytes(int64_t value, size_t repeat);
  // Whether `listpack` (null for an empty one) stays within the size limit
  // of 1 GiB after growing by `bytes`.
  static bool SafeToAdd(const Listpack* listpack, size_t bytes);
  ~Listpack() = default;

 private:
  static constexpr uint16_t kListpackUnknownElementCount = UINT16_MAX;
  static constexpr int kUint7BitIntMax = 127;
  static constexpr int kInt24BitIntMax = (1 << 23) - 1;
  static constexpr int kInt24BitIntMin = -(1 << 23);
  // Keep reallocations below the unsigned 32-bit total-bytes limit.
  static constexpr uint32_t kListpackMaxSafetySize = 1 << 30;
  static constexpr int64_t kListpackEof = 0xff;
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
  // Redis' lpEncodeBacklen: a backlen of at most 127 takes one byte, one
  // below each limit two, three, or four bytes, and any other five. A
  // backlen equal to a limit takes a byte more than it needs, as in every
  // Redis release but 8.8 to 8.10.
  enum BacklenThreshold : uint64_t {
    kSize1ByteBacklenMax = (1ULL << 7) - 1,
    kSize2BytesBacklenLimit = (1ULL << 14) - 1,
    kSize3BytesBacklenLimit = (1ULL << 21) - 1,
    kSize4BytesBacklenLimit = (1ULL << 28) - 1,
  };
  struct Encoding {
    std::string_view string_value;
    int64_t integer_value;
    EncodingGeneralType encoding_type;
    size_t backlen_bytes;
  };
  unsigned char* StringAt(size_t offset, size_t* length,
                          EncodingType encoding_type) const;
  // The integer at `offset`, whose encoding is `encoding_type`, an integer
  // one.
  int64_t DecodeInteger(size_t offset, EncodingType encoding_type) const;
  // Writes `value` as text into `destination`, which holds
  // kListpackIntBufferSize bytes, sets `length` if given, and returns
  // `destination`.
  static unsigned char* FormatInteger(int64_t value, unsigned char* destination,
                                      size_t* length);
  bool Insert(size_t offset, Listpack::Position where,
              const std::string_view* element_string,
              const int64_t* element_integer);
  bool BatchInsert(size_t offset, Listpack::Position where,
                   const ListpackEntry* entries, size_t entry_count);
  // Chooses the encoding for `entry`; returns false if it is too large.
  static bool PlanEntry(const ListpackEntry& entry, Encoding* encoding);
  static size_t EncodedEntryBytes(const Encoding& encoding);
  void SetTotalBytes(uint32_t listpack_bytes);
  uint16_t ElementCount() const;
  void SetNumOfElements(uint16_t element_count) const;
  size_t Skip(size_t offset) const;
  EncodingType EncodingAt(size_t offset) const;
  size_t Backlen(size_t offset) const;
  // Bytes of the element at `offset` before its backlen, given its encoding.
  size_t EncodedBytes(size_t offset, EncodingType encoding_type) const;
  // An element's value bytes and the offset just past it, which is the EOF
  // byte for the last element.
  struct DecodedEntry {
    const unsigned char* data;
    size_t length;
    size_t next;
  };
  // Decodes the element at `offset`, which must not be EOF, reading its
  // encoding once for both the value and the next offset. Integers are rendered
  // into `int_buffer`, which must hold kListpackIntBufferSize bytes. Iterators
  // use this instead of Get() followed by Next(), which decode each element
  // twice. Always inlined, since it runs once per element of every
  // iteration: GCC's link-time optimization otherwise calls it once its
  // budget for inlining across the program runs out.
  [[gnu::always_inline]] DecodedEntry DecodeEntry(
      size_t offset, unsigned char* int_buffer) const;
  // DecodeEntry for everything but strings under 4096 bytes.
  DecodedEntry DecodeLongOrIntegerEntry(size_t offset,
                                        unsigned char* int_buffer) const;
  // Returns the offset of element `position`, which must be below `size`,
  // the element count. Walks from whichever end is closer.
  size_t Seek(size_t position, size_t size) const;
  // Offset of the element before the one at `offset`, which may be the EOF
  // byte; `offset` must not be the first element.
  size_t PrevUnchecked(size_t offset) const;
  static std::string_view ValueView(const DecodedEntry& entry) {
    return {reinterpret_cast<const char*>(entry.data), entry.length};
  }
  static uint8_t BacklenBytes(size_t backlen);
  static size_t EncodeString(unsigned char* buffer, std::string_view element);
  static size_t EncodeInteger(unsigned char* buffer, int64_t value);
  static void EncodeBacklen(unsigned char* buffer, size_t backlen);
  size_t DecodeBacklen(size_t offset) const;
  size_t DecodeStringLength(size_t offset) const;
  static bool IsString(EncodingType encoding_type);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): packed byte storage
  Listpack(std::unique_ptr<unsigned char[]> listpack, size_t capacity)
      : listpack_(std::move(listpack)), capacity_(capacity) {}
  // Replaces the `removed_bytes` at `offset` with `inserted_bytes` of space
  // for the caller to fill, moves the bytes after them, and sets the total
  // size. Keeps the storage while the result fits and uses at least a quarter
  // of it.
  void ResizeGap(size_t offset, size_t removed_bytes, size_t inserted_bytes);
  // ResizeGap into new storage, sized for `new_bytes` with room to grow into.
  void ResizeGapInNewStorage(size_t offset, size_t tail_offset,
                             size_t inserted_bytes, size_t new_bytes);
  std::unique_ptr<unsigned char[]> listpack_;
  // Allocated bytes of listpack_; at least TotalBytes().
  size_t capacity_;
};

inline Listpack::DecodedEntry Listpack::DecodeEntry(
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
    // Their encoded size is at most 4097 bytes, well under the 16383 where
    // a backlen takes a third byte, so the backlen is one byte or two.
    // Worked out here instead of with BacklenBytes, which is out of line
    // and would cost a call for each such string.
    static_assert(2 + 0xfff < (1ULL << 14) - 1);
    const size_t backlen_bytes =
        encoded_bytes <= BacklenThreshold::kSize1ByteBacklenMax ? 1 : 2;
    return {buffer + 2, length, offset + encoded_bytes + backlen_bytes};
  }
  return DecodeLongOrIntegerEntry(offset, int_buffer);
}

template <typename Visitor>
bool Listpack::ForEach(size_t start, size_t stop, Visitor&& visitor) const {
  const size_t size = Size();
  if (start > stop || start >= size) {
    return true;
  }
  stop = std::min(stop, size - 1);

  size_t offset = Seek(start, size);
  // Integers are formatted into a buffer of this call's own, so a visitor
  // may read the listpack again without changing the value it was given.
  std::array<unsigned char, kListpackIntBufferSize> int_buffer{};
  for (size_t index = start; index <= stop && listpack_[offset] != kListpackEof;
       ++index) {
    const DecodedEntry entry = DecodeEntry(offset, int_buffer.data());
    if (!visitor(ValueView(entry))) {
      return false;
    }
    offset = entry.next;
  }
  return true;
}

template <typename Visitor>
bool Listpack::ForEachReverse(size_t start, size_t stop,
                              Visitor&& visitor) const {
  const size_t size = Size();
  if (start > stop || start >= size) {
    return true;
  }
  stop = std::min(stop, size - 1);

  size_t index = stop;
  std::optional<size_t> offset = Seek(stop, size);
  std::array<unsigned char, kListpackIntBufferSize> int_buffer{};
  while (offset.has_value() && index >= start) {
    if (!visitor(ValueView(DecodeEntry(*offset, int_buffer.data())))) {
      return false;
    }
    if (index == 0) {
      break;
    }
    offset = Prev(*offset);
    --index;
  }
  return true;
}

template <typename Visitor>
bool Listpack::ForEachPair(Visitor&& visitor) const {
  const std::optional<size_t> first = First();
  if (!first.has_value()) {
    return true;
  }
  std::array<unsigned char, kListpackIntBufferSize> first_int_buffer{};
  std::array<unsigned char, kListpackIntBufferSize> second_int_buffer{};
  size_t offset = *first;
  while (listpack_[offset] != kListpackEof) {
    const DecodedEntry first_entry =
        DecodeEntry(offset, first_int_buffer.data());
    if (listpack_[first_entry.next] == kListpackEof) {
      break;
    }
    const DecodedEntry second_entry =
        DecodeEntry(first_entry.next, second_int_buffer.data());
    if (!visitor(ValueView(first_entry), ValueView(second_entry))) {
      return false;
    }
    offset = second_entry.next;
  }
  return true;
}
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_LISTPACK_H_
