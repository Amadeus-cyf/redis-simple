#include "containers/quicklist.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/check.h"

namespace redis_simple::containers {
namespace {
bool HasRemoveLimit(size_t limit) { return limit != 0; }

}  // namespace

Quicklist::Node::Node()
    : listpack(std::make_unique<Listpack>()), next(nullptr), prev(nullptr) {}

Quicklist::Quicklist() : Quicklist(kDefaultNodeMaxBytes) {}

Quicklist::Quicklist(size_t node_max_bytes)
    : head_(nullptr),
      tail_(nullptr),
      size_(0),
      node_count_(0),
      node_max_bytes_(
          std::max(node_max_bytes,
                   static_cast<size_t>(Listpack::kListpackHeaderSize + 1))) {}

bool Quicklist::LPush(std::string_view value) {
  if (!head_ || !CanAppendToNode(head_.get(), value)) {
    PrependNode();
  }
  if (!PushToHeadNode(value)) {
    return false;
  }
  ++size_;
  return true;
}

bool Quicklist::RPush(std::string_view value) {
  if (tail_ == nullptr || !CanAppendToNode(tail_, value)) {
    AppendNode();
  }
  if (!PushToTailNode(value)) {
    return false;
  }
  ++size_;
  return true;
}

std::optional<std::string> Quicklist::LPop() {
  if (!head_) {
    return std::nullopt;
  }

  const std::optional<size_t> offset = head_->listpack->First();
  if (!offset.has_value()) {
    return std::nullopt;
  }
  std::optional<std::string> value = head_->listpack->Get(*offset);
  head_->listpack->Delete(*offset);
  --size_;
  if (head_->listpack->Size() == 0) {
    DeleteNode(head_.get());
  }
  MergeNext(head_.get());
  return value;
}

std::optional<std::string> Quicklist::RPop() {
  if (tail_ == nullptr) {
    return std::nullopt;
  }

  Node* node = tail_;
  const std::optional<size_t> offset = node->listpack->Last();
  if (!offset.has_value()) {
    return std::nullopt;
  }
  std::optional<std::string> value = node->listpack->Get(*offset);
  node->listpack->Delete(*offset);
  --size_;
  if (node->listpack->Size() == 0) {
    DeleteNode(node);
    MergeNext(tail_ != nullptr ? tail_->prev : nullptr);
  } else if (node->prev != nullptr) {
    MergeNext(node->prev);
  }
  return value;
}

std::vector<std::string> Quicklist::Range(size_t start, size_t stop) const {
  std::vector<std::string> values;
  if (start > stop || start >= size_) {
    return values;
  }
  stop = std::min(stop, size_ - 1);
  values.reserve(stop - start + 1);
  ForEach(start, stop, [&values](std::string_view value) {
    values.emplace_back(value);
    return true;
  });
  return values;
}

bool Quicklist::Set(size_t index, std::string_view value) {
  const std::optional<EntryLocation> location = Locate(index);
  if (!location.has_value()) {
    return false;
  }
  const std::optional<size_t> offset =
      location->node->listpack->OffsetAt(location->local_index);
  if (!offset.has_value() ||
      !location->node->listpack->Replace(*offset, value)) {
    return false;
  }
  if (!NormalizeNodeSizes(location->node)) {
    return false;
  }
  MergeAround(location->node);
  return true;
}

std::optional<size_t> Quicklist::Remove(std::string_view value, size_t limit,
                                        RemoveDirection direction) {
  if (size_ == 0) {
    return 0;
  }

  size_t removed = 0;
  if (direction == RemoveDirection::kFromTail && HasRemoveLimit(limit)) {
    for (Node* node = tail_; node != nullptr && removed < limit;) {
      Node* prev = node->prev;
      const size_t node_removed =
          node->listpack->DeleteMatching(value, limit - removed, true);
      removed += node_removed;
      size_ -= node_removed;
      if (node->listpack->Size() == 0) {
        DeleteNode(node);
      }
      node = prev;
    }
    MergeAll();
    return removed;
  }

  for (Node* node = head_.get();
       node != nullptr && (!HasRemoveLimit(limit) || removed < limit);) {
    Node* next = node->next.get();
    const size_t node_limit =
        HasRemoveLimit(limit) ? limit - removed : size_t{0};
    const size_t node_removed =
        node->listpack->DeleteMatching(value, node_limit);
    removed += node_removed;
    size_ -= node_removed;
    if (node->listpack->Size() == 0) {
      DeleteNode(node);
    }
    node = next;
  }
  MergeAll();
  return removed;
}

bool Quicklist::Trim(size_t start, size_t stop) {
  if (size_ == 0 || start > stop || start >= size_) {
    Clear();
    return true;
  }

  stop = std::min(stop, size_ - 1);
  size_t suffix_count = size_ - stop - 1;
  while (tail_ != nullptr && suffix_count >= tail_->listpack->Size()) {
    const size_t node_size = tail_->listpack->Size();
    suffix_count -= node_size;
    size_ -= node_size;
    DeleteNode(tail_);
  }
  if (suffix_count > 0) {
    const std::optional<size_t> first_deleted =
        tail_->listpack->OffsetAt(tail_->listpack->Size() - suffix_count);
    if (!first_deleted.has_value() ||
        tail_->listpack->DeleteRange(*first_deleted, suffix_count) !=
            suffix_count) {
      return false;
    }
    size_ -= suffix_count;
  }

  size_t prefix_count = start;
  while (head_ != nullptr && prefix_count >= head_->listpack->Size()) {
    const size_t node_size = head_->listpack->Size();
    prefix_count -= node_size;
    size_ -= node_size;
    DeleteNode(head_.get());
  }
  if (prefix_count > 0) {
    const std::optional<size_t> first = head_->listpack->First();
    if (!first.has_value() ||
        head_->listpack->DeleteRange(*first, prefix_count) != prefix_count) {
      return false;
    }
    size_ -= prefix_count;
  }
  MergeAround(head_.get());
  if (tail_ != nullptr) {
    MergeAround(tail_->prev != nullptr ? tail_->prev : tail_);
  }
  return true;
}

std::optional<Quicklist::EntryLocation> Quicklist::Locate(size_t index) const {
  if (index >= size_) {
    return std::nullopt;
  }

  if (index < size_ / 2) {
    size_t first_index = 0;
    for (Node* node = head_.get(); node != nullptr; node = node->next.get()) {
      const size_t node_size = node->listpack->Size();
      if (index - first_index < node_size) {
        return EntryLocation{node, index - first_index};
      }
      first_index += node_size;
    }
    return std::nullopt;
  }

  size_t end_index = size_;
  for (Node* node = tail_; node != nullptr; node = node->prev) {
    const size_t node_size = node->listpack->Size();
    const size_t first_index = end_index - node_size;
    if (index >= first_index) {
      return EntryLocation{node, index - first_index};
    }
    end_index = first_index;
  }
  return std::nullopt;
}

std::optional<size_t> Quicklist::ListpackBytes() const {
  if (node_count_ > 1) {
    return std::nullopt;
  }
  return head_ == nullptr
             ? std::optional<size_t>(Listpack::kListpackHeaderSize + 1)
             : std::optional<size_t>(head_->listpack->TotalBytes());
}

std::unique_ptr<Listpack> Quicklist::ReleaseListpack() {
  if (node_count_ > 1) {
    return nullptr;
  }
  auto listpack = head_ == nullptr ? std::make_unique<Listpack>()
                                   : std::move(head_->listpack);
  Clear();
  return listpack;
}

bool Quicklist::AdoptListpack(std::unique_ptr<Listpack> listpack) {
  RS_CHECK(head_ == nullptr && listpack != nullptr);
  if (listpack->Size() == 0) {
    return true;
  }
  size_ = listpack->Size();
  Node* const node = AppendNode();
  node->listpack = std::move(listpack);
  return NormalizeNodeSizes(node);
}

bool Quicklist::PushToHeadNode(std::string_view value) {
  return head_ && head_->listpack->Prepend(value);
}

bool Quicklist::PushToTailNode(std::string_view value) {
  return tail_ != nullptr && tail_->listpack->Append(value);
}

bool Quicklist::CanAppendToNode(const Node* node,
                                std::string_view value) const {
  if (node == nullptr) {
    return false;
  }
  const size_t estimated_bytes = Listpack::EstimateEntryBytes(value);
  return node->listpack->Size() == 0 ||
         (node->listpack->TotalBytes() <= node_max_bytes_ &&
          estimated_bytes <= node_max_bytes_ - node->listpack->TotalBytes());
}

bool Quicklist::CanMergeNodes(const Node* left, const Node* right) const {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  const size_t merged_bytes = left->listpack->TotalBytes() +
                              right->listpack->TotalBytes() -
                              Listpack::kListpackHeaderSize - 1;
  return merged_bytes <= node_max_bytes_;
}

bool Quicklist::NormalizeNodeSizes(Node* node) {
  // Only `node` changed, and splitting it inserts the pieces before its
  // original successor, so the scan stops there instead of at the tail.
  const Node* const end = node->next.get();
  for (Node* current = node; current != end; current = current->next.get()) {
    while (current->listpack->TotalBytes() > node_max_bytes_ &&
           current->listpack->Size() > 1) {
      if (SplitNode(current) == nullptr) {
        return false;
      }
    }
  }
  return true;
}

Quicklist::Node* Quicklist::SplitNode(Node* node) {
  if (node == nullptr || node->listpack->Size() <= 1) {
    return nullptr;
  }

  const size_t node_size = node->listpack->Size();
  const size_t split_index = node_size / 2;

  auto right = std::make_unique<Node>();
  if (!node->listpack->ForEach(split_index, node_size - 1,
                               [&right](std::string_view value) {
                                 return right->listpack->Append(value);
                               })) {
    return nullptr;
  }

  const std::optional<size_t> offset = node->listpack->OffsetAt(split_index);
  if (!offset.has_value() ||
      node->listpack->DeleteRange(*offset, node_size - split_index) !=
          node_size - split_index) {
    return nullptr;
  }
  return InsertNodeAfter(node, std::move(right));
}

Quicklist::Node* Quicklist::InsertNodeAfter(Node* node,
                                            std::unique_ptr<Node> new_node) {
  if (node == nullptr || new_node == nullptr) {
    return nullptr;
  }

  Node* inserted = new_node.get();
  new_node->prev = node;
  new_node->next = std::move(node->next);
  if (new_node->next) {
    new_node->next->prev = inserted;
  } else {
    tail_ = inserted;
  }
  node->next = std::move(new_node);
  ++node_count_;
  return inserted;
}

void Quicklist::MergeNext(Node* left) {
  if (!CanMergeNodes(left, left == nullptr ? nullptr : left->next.get())) {
    return;
  }

  Node* right = left->next.get();
  const size_t right_size = right->listpack->Size();
  if (right_size > 0 && !right->listpack->ForEach(
                            0, right_size - 1, [left](std::string_view value) {
                              return left->listpack->Append(value);
                            })) {
    return;
  }
  DeleteNode(right);
}

void Quicklist::MergeAround(Node* node) {
  if (node == nullptr) {
    return;
  }
  if (node->prev != nullptr && CanMergeNodes(node->prev, node)) {
    node = node->prev;
    MergeNext(node);
  }
  while (CanMergeNodes(node, node->next.get())) {
    MergeNext(node);
  }
}

void Quicklist::MergeAll() {
  for (Node* node = head_.get(); node != nullptr; node = node->next.get()) {
    while (CanMergeNodes(node, node->next.get())) {
      MergeNext(node);
    }
  }
}

Quicklist::Node* Quicklist::AppendNode() {
  auto node = std::make_unique<Node>();
  Node* inserted = node.get();
  if (!head_) {
    head_ = std::move(node);
    tail_ = inserted;
  } else {
    RS_CHECK(tail_ != nullptr);
    node->prev = tail_;
    tail_->next = std::move(node);
    tail_ = inserted;
  }
  ++node_count_;
  return inserted;
}

Quicklist::Node* Quicklist::PrependNode() {
  auto node = std::make_unique<Node>();
  Node* inserted = node.get();
  if (!head_) {
    head_ = std::move(node);
    tail_ = inserted;
  } else {
    node->next = std::move(head_);
    node->next->prev = inserted;
    head_ = std::move(node);
  }
  ++node_count_;
  return inserted;
}

void Quicklist::DeleteNode(Node* node) {
  if (node == nullptr) {
    return;
  }

  if (node == head_.get()) {
    head_ = std::move(head_->next);
    if (head_) {
      head_->prev = nullptr;
    } else {
      tail_ = nullptr;
    }
  } else {
    Node* prev = node->prev;
    RS_CHECK(prev != nullptr);
    prev->next = std::move(node->next);
    if (prev->next) {
      prev->next->prev = prev;
    } else {
      tail_ = prev;
    }
  }
  --node_count_;
}

void Quicklist::Clear() {
  // Unlink node by node. Destroying the head alone would destroy each node
  // from inside the one before it, a stack frame per node, which overflows
  // the stack for a long enough list.
  while (head_ != nullptr) {
    head_ = std::move(head_->next);
  }
  tail_ = nullptr;
  size_ = 0;
  node_count_ = 0;
}
}  // namespace redis_simple::containers
