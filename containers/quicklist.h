#ifndef REDIS_SIMPLE_CONTAINERS_QUICKLIST_H_
#define REDIS_SIMPLE_CONTAINERS_QUICKLIST_H_

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "containers/listpack.h"

namespace redis_simple::containers {
class Quicklist {
 public:
  static constexpr size_t kDefaultNodeMaxBytes = 8192;
  enum class RemoveDirection {
    kFromHead,
    kFromTail,
  };

  Quicklist();
  explicit Quicklist(size_t node_max_bytes);
  Quicklist(const Quicklist&) = delete;
  Quicklist& operator=(const Quicklist&) = delete;

  bool LPush(std::string_view value);
  bool RPush(std::string_view value);
  std::optional<std::string> LPop();
  std::optional<std::string> RPop();
  bool Set(size_t index, std::string_view value);
  std::optional<size_t> Remove(std::string_view value, size_t limit,
                               RemoveDirection direction);
  bool Trim(size_t start, size_t stop);
  std::vector<std::string> Range(size_t start, size_t stop) const;
  template <typename Visitor>
  bool ForEach(size_t start, size_t stop, Visitor&& visitor) const;
  template <typename Visitor>
  bool ForEachReverse(size_t start, size_t stop, Visitor&& visitor) const;

  bool Empty() const { return size_ == 0; }
  size_t Size() const { return size_; }
  size_t NodeCount() const { return node_count_; }
  // Calls `visitor(const Listpack&)` for each node, head to tail, until it
  // returns false.
  template <typename Visitor>
  bool ForEachNode(Visitor&& visitor) const {
    for (const Node* node = head_.get(); node != nullptr;
         node = node->next.get()) {
      if (!visitor(*node->listpack)) {
        return false;
      }
    }
    return true;
  }
  std::optional<size_t> ListpackBytes() const;
  std::unique_ptr<Listpack> ReleaseListpack();
  // Takes `listpack` as the only node of this empty list without re-encoding
  // its elements, splitting it if it exceeds the node size limit. This is the
  // inverse of ReleaseListpack, like Redis' quicklistAppendListpack.
  bool AdoptListpack(std::unique_ptr<Listpack> listpack);

 private:
  struct Node {
    Node();

    std::unique_ptr<Listpack> listpack;
    std::unique_ptr<Node> next;
    Node* prev;
  };

  struct EntryLocation {
    Node* node;
    size_t local_index;
  };

  // Finds the node holding element `index`, walking from whichever end of
  // the list is closer, as Redis' quicklistInitIteratorAtIdx does.
  std::optional<EntryLocation> Locate(size_t index) const;
  bool PushToHeadNode(std::string_view value);
  bool PushToTailNode(std::string_view value);
  bool CanAppendToNode(const Node* node, std::string_view value) const;
  bool CanMergeNodes(const Node* left, const Node* right) const;
  // Splits `node` until every piece fits the node size limit.
  bool NormalizeNodeSizes(Node* node);
  Node* SplitNode(Node* node);
  Node* InsertNodeAfter(Node* node, std::unique_ptr<Node> new_node);
  void MergeNext(Node* left);
  void MergeAround(Node* node);
  void MergeAll();
  Node* AppendNode();
  Node* PrependNode();
  void DeleteNode(Node* node);
  void Clear();

  std::unique_ptr<Node> head_;
  Node* tail_;
  size_t size_;
  size_t node_count_;
  size_t node_max_bytes_;
};

template <typename Visitor>
bool Quicklist::ForEach(size_t start, size_t stop, Visitor&& visitor) const {
  if (start > stop || start >= size_) {
    return true;
  }
  stop = std::min(stop, size_ - 1);

  const std::optional<EntryLocation> location = Locate(start);
  if (!location.has_value()) {
    return true;
  }
  // `index` is the list position of the current node's first element.
  size_t index = start - location->local_index;
  for (const Node* node = location->node; node != nullptr && index <= stop;
       node = node->next.get()) {
    const size_t node_size = node->listpack->Size();
    if (node_size == 0) {
      continue;
    }
    const size_t local_start = start > index ? start - index : 0;
    const size_t local_stop = std::min(stop - index, node_size - 1);
    if (!node->listpack->ForEach(local_start, local_stop, visitor)) {
      return false;
    }
    index += node_size;
  }
  return true;
}

template <typename Visitor>
bool Quicklist::ForEachReverse(size_t start, size_t stop,
                               Visitor&& visitor) const {
  if (start > stop || start >= size_) {
    return true;
  }
  stop = std::min(stop, size_ - 1);

  const std::optional<EntryLocation> location = Locate(stop);
  if (!location.has_value()) {
    return true;
  }
  const Node* node = location->node;
  // List position of the current node's first element.
  size_t node_start = stop - location->local_index;
  while (true) {
    const size_t node_stop = node_start + node->listpack->Size() - 1;
    const size_t local_start = start > node_start ? start - node_start : 0;
    const size_t local_stop = std::min(stop, node_stop) - node_start;
    if (!node->listpack->ForEachReverse(local_start, local_stop, visitor)) {
      return false;
    }
    if (node_start <= start) {
      return true;
    }
    do {
      node = node->prev;
    } while (node != nullptr && node->listpack->Size() == 0);
    if (node == nullptr) {
      return true;
    }
    node_start -= node->listpack->Size();
  }
}
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_QUICKLIST_H_
