#ifndef REDIS_SIMPLE_DATA_TYPES_SET_SET_H_
#define REDIS_SIMPLE_DATA_TYPES_SET_SET_H_

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "containers/dict.h"
#include "containers/intset.h"
#include "containers/listpack.h"
#include "utils/check.h"

namespace redis_simple::data_types {
class Set {
 public:
  enum class Encoding {
    kIntset,
    kListpack,
    kDict,
  };

  // Redis 8's set-max-intset-entries, set-max-listpack-entries, and
  // set-max-listpack-value.
  static constexpr size_t kIntsetMaxEntries = 512;
  static constexpr size_t kListpackMaxEntries = 128;
  static constexpr size_t kListpackMaxElementLength = 64;

  static std::unique_ptr<Set> Create() {
    return std::unique_ptr<Set>(new Set());
  }
  // An empty set in the encoding `count` members need, as Redis' rdbLoad
  // creates one: a dict sized for them when `count` is over
  // kIntsetMaxEntries, more than an intset or a listpack holds.
  static std::unique_ptr<Set> CreateForSize(size_t count);
  // A listpack-encoded set holding `listpack`'s elements, which must be
  // distinct, at most kListpackMaxEntries, and no longer than
  // kListpackMaxElementLength, as the RDB loader checks.
  static std::unique_ptr<Set> CreateFromListpack(
      std::unique_ptr<containers::Listpack> listpack);
  bool Add(std::string_view value);
  bool HasMember(std::string_view value) const;
  // As HasMember, but a dict-encoded set probes with `hashed`, when given, so
  // the member is not hashed again. `hashed` must describe `value`.
  bool HasMember(std::string_view value,
                 const containers::HashedKey* hashed) const;
  std::vector<std::string> ListAllMembers() const;
  template <typename Visitor>
  bool ForEachMember(Visitor&& visitor) const;
  // Like ForEachMember, but also passes each member's stored hash as a
  // `const containers::HashedKey*` when the set is dict-encoded, or nullptr
  // otherwise. Set operations hand it to HasMember on the other sets.
  template <typename Visitor>
  bool ForEachMemberWithHash(Visitor&& visitor) const;
  bool Delete(std::string_view value);
  // One SSCAN step over a dict-encoded set (containers::Dict::ScanStep): calls
  // `visitor(member)` for each member it visits and returns the next cursor,
  // 0 when done. Smaller encodings are scanned whole through ForEachMember,
  // as Redis does.
  template <typename Visitor>
  size_t ScanDict(size_t cursor, size_t count, Visitor&& visitor);
  size_t Size() const;
  Encoding Encoding() const { return encoding_; }
  // The encoded forms RDB files store. A listpack-encoded set's listpack,
  // in Redis' byte format.
  std::string_view ListpackBytes() const;
  // An intset-encoded set in Redis' intset layout, appended to `out`.
  void AppendIntsetBlob(std::string* out) const;

 private:
  Set();
  bool IntsetAddAndMaybeConvert(std::string_view value);
  bool ListpackAddAndMaybeConvert(std::string_view value);
  bool DictAdd(std::string_view value);
  void MaybeConvertIntsetToDict();
  void ConvertIntsetToDict(size_t capacity);
  bool MaybeConvertIntsetToListpack(std::string_view value);
  void ConvertIntsetToListpack(std::string_view value);
  void ConvertListpackToDict(size_t capacity);
  enum Encoding encoding_;
  std::unique_ptr<containers::Intset> intset_;
  std::unique_ptr<containers::Listpack> listpack_;
  std::unique_ptr<containers::Dict<std::string, std::nullptr_t>> dict_;
};

template <typename Visitor>
bool Set::ForEachMember(Visitor&& visitor) const {
  const size_t size = Size();
  if (size == 0) {
    return true;
  }
  if (encoding_ == Encoding::kIntset) {
    auto it = containers::Intset::Iterator(intset_.get());
    it.SeekToFirst();
    while (it.Valid()) {
      std::array<char, std::numeric_limits<int64_t>::digits10 + 3> buffer{};
      const auto result = std::to_chars(
          buffer.data(), buffer.data() + buffer.size(), it.Value());
      if (result.ec != std::errc() ||
          !visitor(std::string_view(
              buffer.data(),
              static_cast<size_t>(result.ptr - buffer.data())))) {
        return false;
      }
      it.Next();
    }
    return true;
  }
  if (encoding_ == Encoding::kListpack) {
    return listpack_->ForEach(0, size - 1, visitor);
  }
  if (encoding_ == Encoding::kDict) {
    auto it =
        containers::Dict<std::string, std::nullptr_t>::Iterator(dict_.get());
    it.SeekToFirst();
    while (it.Valid()) {
      if (!visitor(it.Key())) {
        return false;
      }
      it.Next();
    }
    return true;
  }
  utils::Unreachable();
}

template <typename Visitor>
size_t Set::ScanDict(size_t cursor, size_t count, Visitor&& visitor) {
  RS_CHECK(encoding_ == Encoding::kDict);
  return dict_->ScanStep(cursor, count,
                         [&visitor](const std::string& member, std::nullptr_t) {
                           visitor(std::string_view(member));
                         });
}

template <typename Visitor>
bool Set::ForEachMemberWithHash(Visitor&& visitor) const {
  if (encoding_ != Encoding::kDict) {
    return ForEachMember([&visitor](std::string_view member) {
      return visitor(member,
                     static_cast<const containers::HashedKey*>(nullptr));
    });
  }
  auto it =
      containers::Dict<std::string, std::nullptr_t>::Iterator(dict_.get());
  it.SeekToFirst();
  while (it.Valid()) {
    const containers::HashedKey hashed = it.Hashed();
    if (!visitor(std::string_view(it.Key()), &hashed)) {
      return false;
    }
    it.Next();
  }
  return true;
}
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_SET_SET_H_
