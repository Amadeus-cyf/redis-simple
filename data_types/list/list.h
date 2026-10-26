#ifndef REDIS_SIMPLE_DATA_TYPES_LIST_LIST_H_
#define REDIS_SIMPLE_DATA_TYPES_LIST_LIST_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "memory/listpack.h"
#include "memory/quicklist.h"

namespace redis_simple::list {
class List {
 public:
  enum class Encoding {
    kListPack,
    kQuickList,
  };
  enum class RemoveDirection {
    kFromHead,
    kFromTail,
  };

  static constexpr size_t kDefaultListMaxListpackBytes =
      in_memory::QuickList::kDefaultNodeMaxBytes;

  static std::unique_ptr<List> Create(
      size_t list_max_listpack_bytes = kDefaultListMaxListpackBytes) {
    return std::unique_ptr<List>(new List(list_max_listpack_bytes));
  }

  bool LPush(std::string_view value);
  bool RPush(std::string_view value);
  std::optional<std::string> RPop();
  std::optional<std::string> LPop();
  std::optional<std::string> At(size_t index) const;
  bool Set(size_t index, std::string_view value);
  std::optional<size_t> Remove(std::string_view value, size_t limit,
                               RemoveDirection direction);
  bool Trim(size_t start, size_t stop);
  size_t Size() const;
  size_t NodeCount() const;
  std::vector<std::string> Range(size_t start, size_t stop) const;
  template <typename Visitor>
  bool ForEach(size_t start, size_t stop, Visitor&& visitor) const;
  template <typename Visitor>
  bool ForEachReverse(size_t start, size_t stop, Visitor&& visitor) const;
  Encoding Encoding() const;

 private:
  explicit List(size_t list_max_listpack_bytes);
  bool Push(std::string_view value, bool head);
  std::optional<std::string> Pop(bool head);
  std::optional<size_t> RemoveFromListPack(std::string_view value, size_t limit,
                                           RemoveDirection direction);
  void TrimListPack(size_t start, size_t stop);
  bool AdoptReplacement(std::unique_ptr<List> replacement);
  bool WouldExceedListpackLimit(std::string_view value) const;
  bool ConvertListPackToQuickList();
  void TryConvertQuickListToListPack();

  std::unique_ptr<in_memory::ListPack> listpack_;
  std::unique_ptr<in_memory::QuickList> quicklist_;
  size_t list_max_listpack_bytes_;
};

template <typename Visitor>
bool List::ForEach(size_t start, size_t stop, Visitor&& visitor) const {
  return listpack_ ? listpack_->ForEach(start, stop, visitor)
                   : quicklist_->ForEach(start, stop, visitor);
}

template <typename Visitor>
bool List::ForEachReverse(size_t start, size_t stop, Visitor&& visitor) const {
  return listpack_ ? listpack_->ForEachReverse(start, stop, visitor)
                   : quicklist_->ForEachReverse(start, stop, visitor);
}
}  // namespace redis_simple::list

#endif  // REDIS_SIMPLE_DATA_TYPES_LIST_LIST_H_
