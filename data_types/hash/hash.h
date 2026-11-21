#ifndef REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_
#define REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "containers/compact_string.h"
#include "containers/dict.h"
#include "containers/listpack.h"
#include "utils/check.h"

namespace redis_simple::data_types {
// A Redis hash: field and value pairs in one listpack while they are few
// and short, and in a dict once they pass either limit below. It never
// converts back to a listpack.
class Hash {
 public:
  enum class Encoding {
    kListpack,
    kDict,
  };

  struct Entry {
    std::string field;
    std::string value;
  };

  // Redis 8's hash-max-listpack-entries and hash-max-listpack-value.
  static constexpr size_t kListpackMaxEntries = 512;
  static constexpr size_t kListpackMaxElementLength = 64;

  static std::unique_ptr<Hash> Create() {
    return std::unique_ptr<Hash>(new Hash());
  }
  // An empty hash in the encoding `count` fields need, as Redis' rdbLoad
  // creates one: a dict sized for them when `count` is over
  // kListpackMaxEntries.
  static std::unique_ptr<Hash> CreateForSize(size_t count);
  // A listpack-encoded hash holding `listpack`'s field and value pairs. The
  // fields must be distinct, the pairs at most kListpackMaxEntries, and
  // every element no longer than kListpackMaxElementLength, as the RDB
  // loader checks.
  static std::unique_ptr<Hash> CreateFromListpack(
      std::unique_ptr<containers::Listpack> listpack);

  // Returns true when `field` is new, false when its value was replaced.
  bool Set(std::string_view field, std::string_view value);
  std::optional<std::string> Get(std::string_view field) const;
  bool Delete(std::string_view field);
  bool Exists(std::string_view field) const;
  size_t Size() const;
  // Frees up to about `max_elements` elements of a large encoding, so a
  // discarded value can be freed in steps (as RedisDb does for UNLINK).
  // Returns true once the rest is cheap to free at once. Between the first
  // call and the one that returns true, only destroy the value.
  bool ClearSome(size_t max_elements);
  std::vector<Entry> Entries() const;
  // Calls `visitor(value)` and returns true when `field` exists. The value
  // view is valid only during the callback invocation.
  template <typename Visitor>
  bool VisitValue(std::string_view field, Visitor&& visitor) const;
  // The ForEach functions stop when the visitor returns false, and return
  // false then. The views are valid only during the callback invocation.
  template <typename Visitor>
  bool ForEachEntry(Visitor&& visitor) const;
  template <typename Visitor>
  bool ForEachField(Visitor&& visitor) const;
  template <typename Visitor>
  bool ForEachValue(Visitor&& visitor) const;
  // One HSCAN step over a dict-encoded hash (containers::Dict::ScanStep): calls
  // `visitor(field, value)` for each entry it visits and returns the next
  // cursor, 0 when done. A listpack is scanned whole through ForEachEntry,
  // as Redis does.
  template <typename Visitor>
  size_t ScanDict(size_t cursor, size_t count, Visitor&& visitor);
  Encoding Encoding() const { return encoding_; }
  // A listpack-encoded hash's listpack, in Redis' byte format, as RDB files
  // store it.
  std::string_view ListpackBytes() const;

 private:
  Hash();
  bool CanAppendToListpack(std::string_view field,
                           std::string_view value) const;
  bool SetListpack(std::string_view field, std::string_view value);
  bool SetDict(std::string_view field, std::string_view value);
  void ConvertListpackToDict(size_t capacity);
  std::optional<size_t> FindListpackField(std::string_view field) const;

  enum Encoding encoding_;
  // The listpack of a listpack-encoded value. Aborts for another encoding,
  // which callers rule out first.
  containers::Listpack& ListpackStorage() {
    if (!listpack_.Allocated()) {
      utils::Unreachable();
    }
    return listpack_;
  }
  const containers::Listpack& ListpackStorage() const {
    if (!listpack_.Allocated()) {
      utils::Unreachable();
    }
    return listpack_;
  }
  // Held by value, so a small hash is no allocation beyond its bytes.
  containers::Listpack listpack_;
  std::unique_ptr<containers::Dict<std::string, containers::CompactString>>
      dict_;
};

template <typename Visitor>
bool Hash::VisitValue(std::string_view field, Visitor&& visitor) const {
  if (encoding_ == Encoding::kListpack) {
    const std::optional<size_t> field_offset = FindListpackField(field);
    if (!field_offset.has_value()) {
      return false;
    }
    const std::optional<size_t> value_offset = listpack_.Next(*field_offset);
    if (!value_offset.has_value()) {
      return false;
    }
    containers::Listpack::IntBuffer buffer;
    const std::optional<std::string_view> value =
        listpack_.ElementAt(*value_offset, &buffer);
    if (!value.has_value()) {
      return false;
    }
    visitor(*value);
    return true;
  }
  if (encoding_ == Encoding::kDict) {
    const auto* value = dict_->FindValue(field);
    if (value == nullptr) {
      return false;
    }
    visitor(value->View());
    return true;
  }
  utils::Unreachable();
}

template <typename Visitor>
bool Hash::ForEachEntry(Visitor&& visitor) const {
  if (Size() == 0) {
    return true;
  }
  if (encoding_ == Encoding::kListpack) {
    return listpack_.ForEachPair(visitor);
  }
  if (encoding_ == Encoding::kDict) {
    auto it =
        containers::Dict<std::string, containers::CompactString>::Iterator(
            dict_.get());
    it.SeekToFirst();
    while (it.Valid()) {
      if (!visitor(it.Key(), it.Value().View())) {
        return false;
      }
      it.Next();
    }
    return true;
  }
  utils::Unreachable();
}

template <typename Visitor>
size_t Hash::ScanDict(size_t cursor, size_t count, Visitor&& visitor) {
  RS_CHECK(encoding_ == Encoding::kDict);
  return dict_->ScanStep(cursor, count,
                         [&visitor](std::string_view field,
                                    const containers::CompactString& value) {
                           visitor(field, value.View());
                         });
}

template <typename Visitor>
bool Hash::ForEachField(Visitor&& visitor) const {
  return ForEachEntry(
      [&visitor](std::string_view field, std::string_view /*value*/) {
        return visitor(field);
      });
}

template <typename Visitor>
bool Hash::ForEachValue(Visitor&& visitor) const {
  return ForEachEntry(
      [&visitor](std::string_view /*field*/, std::string_view value) {
        return visitor(value);
      });
}
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_
