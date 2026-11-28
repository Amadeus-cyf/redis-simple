#ifndef REDIS_SIMPLE_DATA_TYPES_LIST_LIST_H_
#define REDIS_SIMPLE_DATA_TYPES_LIST_LIST_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "containers/listpack.h"
#include "containers/quicklist.h"
#include "utils/check.h"

namespace redis_simple::data_types {
// A Redis list: one listpack while it fits in `list_max_listpack_bytes`,
// and a quicklist (a linked list of listpacks) once it does not. It
// converts back when one node holds at most half that limit.
class List {
 public:
  enum class Encoding {
    kListpack,
    kQuicklist,
  };

  static constexpr size_t kDefaultListMaxListpackBytes =
      containers::Quicklist::kDefaultNodeMaxBytes;

  static std::unique_ptr<List> Create(
      size_t list_max_listpack_bytes = kDefaultListMaxListpackBytes) {
    return std::unique_ptr<List>(new List(list_max_listpack_bytes));
  }

  bool LPush(std::string_view value);
  bool RPush(std::string_view value);
  std::optional<std::string> RPop();
  std::optional<std::string> LPop();
  // Removes the element at `end` after calling `visitor(std::string_view)`
  // with it, so a caller can use the value without copying it out first.
  // The view is valid only during the call. Returns false, calling nothing,
  // when the list is empty.
  template <typename Visitor>
  bool Pop(containers::ListEnd end, Visitor&& visitor);
  std::optional<std::string> At(size_t index) const;
  bool Set(size_t index, std::string_view value);
  // Removes elements equal to `value`, at most `limit` of them unless it is
  // zero, starting from the end `from`. Returns how many were removed.
  size_t Remove(std::string_view value, size_t limit, containers::ListEnd from);
  // Keeps the elements at indexes `start` to `stop`, both included, and
  // removes the rest; an empty or out-of-range interval empties the list.
  void Trim(size_t start, size_t stop);
  size_t Size() const;
  // The number of quicklist nodes, 0 for a listpack encoding.
  size_t NodeCount() const;
  std::vector<std::string> Range(size_t start, size_t stop) const;
  // Call `visitor(std::string_view value)` for the elements at indexes
  // `start` to `stop`, both included, until it returns false.
  template <typename Visitor>
  bool ForEach(size_t start, size_t stop, Visitor&& visitor) const;
  template <typename Visitor>
  bool ForEachReverse(size_t start, size_t stop, Visitor&& visitor) const;
  Encoding Encoding() const;
  // Calls `visitor(std::string_view bytes)` with each node's listpack in
  // Redis' byte format, as RDB files store a list: one for a listpack
  // encoding, one per quicklist node otherwise. Stops when it returns false.
  template <typename Visitor>
  bool ForEachListpack(Visitor&& visitor) const {
    if (listpack_.Allocated()) {
      return visitor(listpack_.Bytes());
    }
    return quicklist_->ForEachNode(
        [&visitor](const containers::Listpack& node) {
          return visitor(node.Bytes());
        });
  }

 private:
  explicit List(size_t list_max_listpack_bytes);
  bool Push(std::string_view value, containers::ListEnd end);
  std::optional<std::string> PopValue(containers::ListEnd end);
  void TrimListpack(size_t start, size_t stop);
  bool WouldExceedListpackLimit(std::string_view value) const;
  void ConvertListpackToQuicklist();
  void TryConvertQuicklistToListpack();

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
  // Held by value, so a small list is no allocation beyond its bytes.
  containers::Listpack listpack_;
  std::unique_ptr<containers::Quicklist> quicklist_;
  size_t list_max_listpack_bytes_;
};

template <typename Visitor>
bool List::Pop(containers::ListEnd end, Visitor&& visitor) {
  if (listpack_.Allocated()) {
    return listpack_.Pop(end, visitor);
  }
  const bool popped = quicklist_->Pop(end, visitor);
  // With one element fewer the list may fit a listpack again.
  TryConvertQuicklistToListpack();
  return popped;
}

template <typename Visitor>
bool List::ForEach(size_t start, size_t stop, Visitor&& visitor) const {
  return listpack_.Allocated() ? listpack_.ForEach(start, stop, visitor)
                               : quicklist_->ForEach(start, stop, visitor);
}

template <typename Visitor>
bool List::ForEachReverse(size_t start, size_t stop, Visitor&& visitor) const {
  return listpack_.Allocated()
             ? listpack_.ForEachReverse(start, stop, visitor)
             : quicklist_->ForEachReverse(start, stop, visitor);
}
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_LIST_LIST_H_
