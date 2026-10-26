#ifndef REDIS_SIMPLE_MEMORY_QUICKLIST_H_
#define REDIS_SIMPLE_MEMORY_QUICKLIST_H_

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "memory/listpack.h"

namespace redis_simple::in_memory {
class QuickList {
 public:
  static constexpr size_t kDefaultNodeMaxBytes = 8192;
  enum class RemoveDirection {
    kFromHead,
    kFromTail,
  };

  QuickList();
  explicit QuickList(size_t node_max_bytes);
  QuickList(const QuickList&) = delete;
  QuickList& operator=(const QuickList&) = delete;

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
  std::optional<size_t> ListPackBytes() const;
  std::unique_ptr<ListPack> ReleaseListPack();

 private:
  struct Node {
    Node();

    std::unique_ptr<ListPack> listpack;
    std::unique_ptr<Node> next;
    Node* prev;
  };

  struct EntryLocation {
    Node* node;
    size_t local_index;
  };

  std::optional<EntryLocation> Locate(size_t index);
  bool PushToHeadNode(std::string_view value);
  bool PushToTailNode(std::string_view value);
  bool CanAppendToNode(const Node* node, std::string_view value) const;
  bool CanMergeNodes(const Node* left, const Node* right) const;
  bool NormalizeNodeSizesFrom(Node* node);
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
bool QuickList::ForEach(size_t start, size_t stop, Visitor&& visitor) const {
  if (start > stop || start >= size_) {
    return true;
  }
  stop = std::min(stop, size_ - 1);

  size_t index = 0;
  for (const Node* node = head_.get(); node != nullptr;
       node = node->next.get()) {
    const size_t node_size = node->listpack->Size();
    if (node_size == 0) {
      continue;
    }
    if (index + node_size <= start) {
      index += node_size;
      continue;
    }

    const size_t local_start = start > index ? start - index : 0;
    const size_t local_stop = std::min(stop - index, node_size - 1);
    if (!node->listpack->ForEach(local_start, local_stop, visitor)) {
      return false;
    }
    index += node_size;
    if (index > stop) {
      break;
    }
  }
  return true;
}

template <typename Visitor>
bool QuickList::ForEachReverse(size_t start, size_t stop,
                               Visitor&& visitor) const {
  if (start > stop || start >= size_) {
    return true;
  }
  stop = std::min(stop, size_ - 1);

  size_t node_start = size_;
  for (const Node* node = tail_; node != nullptr; node = node->prev) {
    const size_t node_size = node->listpack->Size();
    if (node_size == 0) {
      continue;
    }
    node_start -= node_size;
    const size_t node_stop = node_start + node_size - 1;
    if (node_start > stop) {
      continue;
    }
    if (node_stop < start) {
      break;
    }

    const size_t local_start = start > node_start ? start - node_start : 0;
    const size_t local_stop = std::min(stop, node_stop) - node_start;
    if (!node->listpack->ForEachReverse(local_start, local_stop, visitor)) {
      return false;
    }
  }
  return true;
}
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_QUICKLIST_H_
