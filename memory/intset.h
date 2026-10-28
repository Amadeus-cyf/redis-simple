#ifndef REDIS_SIMPLE_MEMORY_INTSET_H_
#define REDIS_SIMPLE_MEMORY_INTSET_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace redis_simple::in_memory {
// An in memory set storing integers in ascending order.
class IntSet {
 public:
  class Iterator;
  IntSet();
  IntSet(const IntSet&) = delete;
  IntSet& operator=(const IntSet&) = delete;
  bool Add(int64_t value);
  int64_t Get(unsigned int index) const;
  bool Find(int64_t value) const;
  bool Remove(int64_t value);
  int64_t Max() const;
  int64_t Min() const;
  unsigned int Size() const { return length_; }
  ~IntSet() = default;

 private:
  static constexpr unsigned kInitSize = 1;
  enum EncodingType {
    kInt16 = sizeof(int16_t),
    kInt32 = sizeof(int32_t),
    kInt64 = sizeof(int64_t),
  };
  static EncodingType ValueEncoding(int64_t value);
  void Resize(unsigned int length_);
  int64_t EncodedValue(unsigned int index, EncodingType encoding) const;
  void UpgradeAndAdd(int64_t value);
  bool Search(int64_t value, unsigned int* const index) const;
  void Set(unsigned int index, int64_t value);
  void MoveTail(unsigned int from, unsigned int to);
  std::unique_ptr<unsigned char[]> contents_;
  unsigned int length_;
  EncodingType encoding_;
};

class IntSet::Iterator {
 public:
  explicit Iterator(const IntSet* intset) : intset_(intset), idx_(0) {}
  bool operator==(const Iterator& it) const = default;
  // Return true if the iterator is positioned at a valid element.
  bool Valid() const { return idx_ < intset_->Size(); }
  // Position at the first element.
  void SeekToFirst() { idx_ = 0; }
  // Position at the last element.
  void SeekToLast() { idx_ = intset_->Size() - 1; }
  // Advance to the next element.
  void Next() { ++idx_; }
  // Advance to the previous element.
  void Prev() { --idx_; }
  int64_t Value() { return intset_->Get(idx_); }

 private:
  const IntSet* intset_;
  size_t idx_;
};
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_INTSET_H_
