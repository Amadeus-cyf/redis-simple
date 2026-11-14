#ifndef REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_
#define REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "containers/dict.h"
#include "containers/listpack.h"
#include "utils/check.h"

namespace redis_simple::data_types {
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

  bool Set(std::string_view field, std::string_view value);
  std::optional<std::string> Get(std::string_view field) const;
  bool Delete(std::string_view field);
  bool Exists(std::string_view field) const;
  size_t Size() const;
  std::vector<Entry> Entries() const;
  // The value view is valid only during the callback invocation.
  template <typename Visitor>
  bool VisitValue(std::string_view field, Visitor&& visitor) const;
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
  std::unique_ptr<containers::Listpack> listpack_;
  std::unique_ptr<containers::Dict<std::string, std::string>> dict_;
};

template <typename Visitor>
bool Hash::VisitValue(std::string_view field, Visitor&& visitor) const {
  if (encoding_ == Encoding::kListpack) {
    const std::optional<size_t> field_offset = FindListpackField(field);
    if (!field_offset.has_value()) {
      return false;
    }
    const std::optional<size_t> value_offset = listpack_->Next(*field_offset);
    if (!value_offset.has_value()) {
      return false;
    }
    size_t length = 0;
    const unsigned char* data = listpack_->Get(*value_offset, &length);
    if (data == nullptr) {
      return false;
    }
    visitor(std::string_view(reinterpret_cast<const char*>(data), length));
    return true;
  }
  if (encoding_ == Encoding::kDict) {
    const auto* value = dict_->FindValue(field);
    if (value == nullptr) {
      return false;
    }
    visitor(std::string_view(value->data(), value->size()));
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
    return listpack_->ForEachPair(visitor);
  }
  if (encoding_ == Encoding::kDict) {
    auto it = containers::Dict<std::string, std::string>::Iterator(dict_.get());
    it.SeekToFirst();
    while (it.Valid()) {
      if (!visitor(it.Key(), it.Value())) {
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
  return dict_->ScanStep(
      cursor, count,
      [&visitor](const std::string& field, const std::string& value) {
        visitor(std::string_view(field), std::string_view(value));
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
