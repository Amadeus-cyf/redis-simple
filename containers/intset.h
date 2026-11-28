#ifndef REDIS_SIMPLE_CONTAINERS_INTSET_H_
#define REDIS_SIMPLE_CONTAINERS_INTSET_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace redis_simple::containers {
// An in-memory set storing integers in ascending order. As in Redis' intset,
// all values share one width (16, 32 or 64 bits), which is widened when a
// larger value is added and never narrowed again.
class Intset {
 public:
  class Iterator;
  Intset();
  // The set stored in `blob`, which is in the layout AppendRedisBlob writes.
  // Null unless the blob passes the checks Redis makes of an intset it loads
  // (intsetValidateIntegrity): a width of 2, 4 or 8 bytes, a size that
  // matches its length, and values in ascending order without duplicates. An
  // empty set is accepted as well. The set keeps the blob's width, as Redis
  // does, even when its values fit a narrower one.
  static std::unique_ptr<Intset> CreateFromRedisBlob(std::string_view blob);
  Intset(const Intset&) = delete;
  Intset& operator=(const Intset&) = delete;
  bool Add(int64_t value);
  int64_t Get(uint32_t index) const;
  bool Find(int64_t value) const;
  bool Delete(int64_t value);
  int64_t Max() const;
  int64_t Min() const;
  uint32_t Size() const { return length_; }
  // Appends the set in Redis' intset layout, as RDB files store it: the
  // encoding's width and the length as 32-bit integers, then the values in
  // ascending order at that width, all little-endian.
  void AppendRedisBlob(std::string* out) const;
  ~Intset() = default;

 private:
  enum EncodingType {
    kInt16 = sizeof(int16_t),
    kInt32 = sizeof(int32_t),
    kInt64 = sizeof(int64_t),
  };
  // A set of `length` values of `encoding`, none of them written yet.
  Intset(uint32_t length, EncodingType encoding);
  static EncodingType ValueEncoding(int64_t value);
  // The bytes allocated for `length` values of `encoding`. contents_ always
  // holds exactly this many for the current length and encoding, so the
  // capacity needs no member of its own.
  static size_t CapacityBytes(uint32_t length, EncodingType encoding);
  // Opens a slot for one more value at `index`.
  void InsertSlot(uint32_t index);
  // Closes the slot of the value at `index`.
  void RemoveSlot(uint32_t index);
  int64_t EncodedValue(uint32_t index, EncodingType encoding) const;
  // The value at `index` of `contents`, an array of `encoding`-wide integers.
  static int64_t DecodeAt(const unsigned char* contents, uint32_t index,
                          EncodingType encoding);
  void UpgradeAndAdd(int64_t value);
  bool Search(int64_t value, uint32_t* index) const;
  void Set(uint32_t index, int64_t value);
  std::unique_ptr<unsigned char[]> contents_;
  uint32_t length_;
  EncodingType encoding_;
};

class Intset::Iterator {
 public:
  explicit Iterator(const Intset* intset) : intset_(intset), index_(0) {}
  bool operator==(const Iterator& it) const = default;
  // Return true if the iterator is positioned at a valid element.
  bool Valid() const { return index_ < intset_->Size(); }
  // Position at the first element.
  void SeekToFirst() { index_ = 0; }
  // Position at the last element.
  void SeekToLast() { index_ = intset_->Size() - 1; }
  // Advance to the next element.
  void Next() { ++index_; }
  // Move back to the previous element.
  void Prev() { --index_; }
  int64_t Value() { return intset_->Get(index_); }

 private:
  const Intset* intset_;
  size_t index_;
};
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_INTSET_H_
