#ifndef REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_
#define REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "memory/dict.h"
#include "memory/listpack.h"

namespace redis_simple::hash {
class Hash {
 public:
  enum class Encoding {
    kListPack,
    kDict,
  };

  struct Entry {
    std::string field;
    std::string value;
  };

  static std::unique_ptr<Hash> Create() {
    return std::unique_ptr<Hash>(new Hash());
  }

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
  Encoding Encoding() const { return encoding_; }

 private:
  Hash();
  bool CanAppendToListPack(std::string_view field,
                           std::string_view value) const;
  bool SetListPack(std::string_view field, std::string_view value);
  bool SetDict(std::string_view field, std::string_view value);
  void ConvertListPackToDict(size_t capacity);
  std::optional<size_t> FindListPackField(std::string_view field) const;

  enum Encoding encoding_;
  std::unique_ptr<in_memory::ListPack> listpack_;
  std::unique_ptr<in_memory::Dict<std::string, std::string>> dict_;
};

template <typename Visitor>
bool Hash::VisitValue(std::string_view field, Visitor&& visitor) const {
  if (encoding_ == Encoding::kListPack) {
    const auto field_idx = FindListPackField(field);
    if (!field_idx.has_value()) {
      return false;
    }
    const auto value_idx = listpack_->Next(*field_idx);
    if (!value_idx.has_value()) {
      return false;
    }
    size_t len = 0;
    const auto* data = listpack_->Get(*value_idx, &len);
    if (data == nullptr) {
      return false;
    }
    visitor(std::string_view(reinterpret_cast<const char*>(data), len));
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
  throw std::invalid_argument("unknown hash encoding type");
}

template <typename Visitor>
bool Hash::ForEachEntry(Visitor&& visitor) const {
  if (Size() == 0) {
    return true;
  }
  if (encoding_ == Encoding::kListPack) {
    return listpack_->ForEachPair(visitor);
  }
  if (encoding_ == Encoding::kDict) {
    auto it = in_memory::Dict<std::string, std::string>::Iterator(dict_.get());
    it.SeekToFirst();
    while (it.Valid()) {
      if (!visitor(it.Key(), it.Value())) {
        return false;
      }
      it.Next();
    }
    return true;
  }
  throw std::invalid_argument("unknown hash encoding type");
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
}  // namespace redis_simple::hash

#endif  // REDIS_SIMPLE_DATA_TYPES_HASH_HASH_H_
