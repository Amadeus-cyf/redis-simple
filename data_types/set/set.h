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

#include "memory/dict.h"
#include "memory/intset.h"
#include "memory/listpack.h"
#include "utils/check.h"

namespace redis_simple::set {
class Set {
 public:
  enum class Encoding {
    kIntSet,
    kListPack,
    kDict,
  };

  static std::unique_ptr<Set> Create() {
    return std::unique_ptr<Set>(new Set());
  }
  bool Add(std::string_view value);
  bool HasMember(std::string_view value) const;
  // As HasMember, but a dict-encoded set probes with `hashed`, when given, so
  // the member is not hashed again. `hashed` must describe `value`.
  bool HasMember(std::string_view value, const memory::HashedKey* hashed) const;
  std::vector<std::string> ListAllMembers() const;
  template <typename Visitor>
  bool ForEachMember(Visitor&& visitor) const;
  // Like ForEachMember, but also passes each member's stored hash as a
  // `const memory::HashedKey*` when the set is dict-encoded, or nullptr
  // otherwise. Set operations hand it to HasMember on the other sets.
  template <typename Visitor>
  bool ForEachMemberWithHash(Visitor&& visitor) const;
  bool Delete(std::string_view value);
  // One SSCAN step over a dict-encoded set (memory::Dict::ScanStep): calls
  // `visitor(member)` for each member it visits and returns the next cursor,
  // 0 when done. Smaller encodings are scanned whole through ForEachMember,
  // as Redis does.
  template <typename Visitor>
  size_t ScanDict(size_t cursor, size_t count, Visitor&& visitor);
  size_t Size() const;
  Encoding Encoding() const { return encoding_; }

 private:
  static constexpr size_t kIntSetMaxEntries = 512;
  static constexpr size_t kListPackMaxEntries = 128;
  static constexpr size_t kListPackElementMaxLength = 64;
  Set();
  bool IntSetAddAndMaybeConvert(std::string_view value);
  bool ListPackAddAndMaybeConvert(std::string_view value);
  bool DictAdd(std::string_view value);
  void MaybeConvertIntsetToDict();
  void ConvertIntSetToDict(size_t capacity);
  bool MaybeConvertIntSetToListPack(std::string_view value);
  void ConvertIntSetToListPack(std::string_view value);
  void ConvertListPackToDict(size_t capacity);
  enum Encoding encoding_;
  std::unique_ptr<memory::IntSet> intset_;
  std::unique_ptr<memory::ListPack> listpack_;
  std::unique_ptr<memory::Dict<std::string, std::nullptr_t>> dict_;
};

template <typename Visitor>
bool Set::ForEachMember(Visitor&& visitor) const {
  const size_t size = Size();
  if (size == 0) {
    return true;
  }
  if (encoding_ == Encoding::kIntSet) {
    auto it = memory::IntSet::Iterator(intset_.get());
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
  if (encoding_ == Encoding::kListPack) {
    return listpack_->ForEach(0, size - 1, visitor);
  }
  if (encoding_ == Encoding::kDict) {
    auto it = memory::Dict<std::string, std::nullptr_t>::Iterator(dict_.get());
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
      return visitor(member, static_cast<const memory::HashedKey*>(nullptr));
    });
  }
  auto it = memory::Dict<std::string, std::nullptr_t>::Iterator(dict_.get());
  it.SeekToFirst();
  while (it.Valid()) {
    const memory::HashedKey hashed = it.Hashed();
    if (!visitor(std::string_view(it.Key()), &hashed)) {
      return false;
    }
    it.Next();
  }
  return true;
}
}  // namespace redis_simple::set

#endif  // REDIS_SIMPLE_DATA_TYPES_SET_SET_H_
