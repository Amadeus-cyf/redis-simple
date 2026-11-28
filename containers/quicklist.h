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
// A list stored as a doubly linked chain of Listpack nodes, each kept within
// `node_max_bytes`, like Redis' quicklist. No node is empty. Unlike Redis, a
// node that elements were removed from or replaced in is merged with a
// neighbor when the two fit together. Only those nodes are tried, so an
// operation near one end does not read the rest of the list.
class Quicklist {
 public:
  static constexpr size_t kDefaultNodeMaxBytes = 8192;

  Quicklist();
  explicit Quicklist(size_t node_max_bytes);
  Quicklist(const Quicklist&) = delete;
  Quicklist& operator=(const Quicklist&) = delete;
  ~Quicklist() { Clear(); }

  bool LPush(std::string_view value);
  bool RPush(std::string_view value);
  std::optional<std::string> LPop();
  std::optional<std::string> RPop();
  // Removes the element at `end` after calling `visitor(std::string_view)`
  // with it, so a caller can use the value without copying it out first.
  // The view is valid only during the call. Returns false, calling nothing,
  // when the list is empty.
  template <typename Visitor>
  bool Pop(ListEnd end, Visitor&& visitor);
  bool Set(size_t index, std::string_view value);
  // Removes elements equal to `value`, at most `limit` of them unless it is
  // zero, starting from the end `from`. Returns how many were removed.
  size_t Remove(std::string_view value, size_t limit, ListEnd from);
  // Keeps only the elements at indexes `start` to `stop`, both included,
  // and empties the list when that range holds none.
  void Trim(size_t start, size_t stop);
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
      if (!visitor(node->listpack)) {
        return false;
      }
    }
    return true;
  }
  // The size in bytes of the listpack ExtractListpack would return, or
  // nullopt when the list has more than one node.
  std::optional<size_t> ListpackBytes() const;
  // Moves the elements out as one listpack, leaving the list empty. The
  // list must have at most one node.
  Listpack ExtractListpack();
  // Takes `listpack` as the only node of this empty list without re-encoding
  // its elements, splitting it if it exceeds the node size limit. This is the
  // inverse of ExtractListpack, like Redis' quicklistAppendListpack.
  void AdoptListpack(Listpack listpack);

 private:
  struct Node {
    // In the node, so a node is one allocation besides its bytes.
    Listpack listpack;
    // The number of elements in `listpack`, like the count in Redis'
    // quicklistNode: an index seek adds these up without reading the bytes
    // of the nodes it passes.
    size_t count{};
    std::unique_ptr<Node> next;
    Node* prev{};
  };

  struct EntryLocation {
    Node* node;
    size_t local_index;
  };

  // Finds the node holding element `index`, walking from whichever end of
  // the list is closer, as Redis' quicklistInitIteratorAtIdx does.
  std::optional<EntryLocation> Locate(size_t index) const;
  std::optional<std::string> PopValue(ListEnd end);
  // Deletes `node`, the node at `end` that an element was just popped from,
  // if that emptied it, and merges the node now at that end with its
  // neighbor if they fit together.
  void MergeAfterPop(Node* node, ListEnd end);
  bool CanAppendToNode(const Node* node, std::string_view value) const;
  bool CanMergeNodes(const Node* left, const Node* right) const;
  // Deletes `count` elements, fewer than `node` holds, from its `end`.
  void DeleteFromNode(Node* node, ListEnd end, size_t count);
  // Splits `node` until every piece fits the node size limit.
  void NormalizeNodeSizes(Node* node);
  // Moves the second half of the elements of `node`, which holds at least
  // two, into a new node after it.
  void SplitNode(Node* node);
  void InsertNodeAfter(Node* node, std::unique_ptr<Node> new_node);
  // Appends the node after `left` to it, if there is one and the two fit
  // together, and deletes that node. Returns whether it merged them.
  bool MergeNext(Node* left);
  // Merges `node` with the node before it and the nodes after it, as far as
  // they fit together.
  void MergeAround(Node* node);
  Node* AppendNode();
  Node* PrependNode();
  void DeleteNode(Node* node);
  void Clear();

  std::unique_ptr<Node> head_;
  Node* tail_{};
  size_t size_{};
  size_t node_count_{};
  size_t node_max_bytes_;
};

template <typename Visitor>
bool Quicklist::Pop(ListEnd end, Visitor&& visitor) {
  Node* const node = end == ListEnd::kHead ? head_.get() : tail_;
  if (node == nullptr || !node->listpack.Pop(end, visitor)) {
    return false;
  }
  --node->count;
  --size_;
  MergeAfterPop(node, end);
  return true;
}

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
    const size_t local_start = start > index ? start - index : 0;
    const size_t local_stop = std::min(stop - index, node->count - 1);
    if (!node->listpack.ForEach(local_start, local_stop, visitor)) {
      return false;
    }
    index += node->count;
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
    const size_t node_stop = node_start + node->count - 1;
    const size_t local_start = start > node_start ? start - node_start : 0;
    const size_t local_stop = std::min(stop, node_stop) - node_start;
    if (!node->listpack.ForEachReverse(local_start, local_stop, visitor)) {
      return false;
    }
    if (node_start <= start) {
      return true;
    }
    node = node->prev;
    if (node == nullptr) {
      return true;
    }
    node_start -= node->count;
  }
}
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_QUICKLIST_H_
