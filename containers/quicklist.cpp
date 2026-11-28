#include "containers/quicklist.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/check.h"

namespace redis_simple::containers {
Quicklist::Quicklist() : Quicklist(kDefaultNodeMaxBytes) {}

Quicklist::Quicklist(size_t node_max_bytes)
    : node_max_bytes_(std::max(node_max_bytes, Listpack::kEmptyListpackBytes)) {
}

bool Quicklist::LPush(std::string_view value) {
  const bool fits = head_ != nullptr && CanAppendToNode(head_.get(), value);
  // A new node takes the element whatever its size.
  Node* const node = fits ? head_.get() : PrependNode();
  if (!node->listpack.Prepend(value)) {
    if (!fits) {
      DeleteNode(node);
    }
    return false;
  }
  ++node->count;
  ++size_;
  return true;
}

bool Quicklist::RPush(std::string_view value) {
  const bool fits = tail_ != nullptr && CanAppendToNode(tail_, value);
  // A new node takes the element whatever its size.
  Node* const node = fits ? tail_ : AppendNode();
  if (!node->listpack.Append(value)) {
    if (!fits) {
      DeleteNode(node);
    }
    return false;
  }
  ++node->count;
  ++size_;
  return true;
}

std::optional<std::string> Quicklist::LPop() {
  return PopValue(ListEnd::kHead);
}

std::optional<std::string> Quicklist::RPop() {
  return PopValue(ListEnd::kTail);
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
      location->node->listpack.OffsetAt(location->local_index);
  if (!offset.has_value() ||
      !location->node->listpack.Replace(*offset, value)) {
    return false;
  }
  NormalizeNodeSizes(location->node);
  MergeAround(location->node);
  return true;
}

size_t Quicklist::Remove(std::string_view value, size_t limit, ListEnd from) {
  const bool limited = limit != 0;
  size_t removed = 0;
  // Whether the node last passed shrank, grew by a merge, or lost a
  // neighbor on the side the walk is heading to, so that it may now fit
  // together with the next node the walk reaches. Only those pairs are
  // tried, so a removal near one end leaves the rest of the list unread.
  bool changed = false;
  // Without a limit every match goes, whichever end the walk starts from.
  if (from == ListEnd::kTail && limited) {
    Node* node = tail_;
    while (node != nullptr && removed < limit) {
      Node* const prev = node->prev;
      const size_t node_removed =
          node->listpack.DeleteMatching(value, limit - removed, ListEnd::kTail);
      removed += node_removed;
      size_ -= node_removed;
      node->count -= node_removed;
      if (node->count == 0) {
        DeleteNode(node);
        changed = true;
      } else if (node_removed > 0 || changed) {
        const bool merged = MergeNext(node);
        changed = node_removed > 0 || merged;
      }
      node = prev;
    }
    if (changed) {
      MergeNext(node);
    }
    return removed;
  }

  Node* node = head_.get();
  while (node != nullptr && (!limited || removed < limit)) {
    Node* const next = node->next.get();
    const size_t node_removed = node->listpack.DeleteMatching(
        value, limited ? limit - removed : 0, ListEnd::kHead);
    removed += node_removed;
    size_ -= node_removed;
    node->count -= node_removed;
    if (node->count == 0) {
      DeleteNode(node);
      changed = true;
    } else if (node_removed > 0 || changed) {
      const bool merged = MergeNext(node->prev);
      changed = node_removed > 0 || merged;
    }
    node = next;
  }
  if (changed && node != nullptr) {
    MergeNext(node->prev);
  }
  return removed;
}

void Quicklist::Trim(size_t start, size_t stop) {
  if (start > stop || start >= size_) {
    Clear();
    return;
  }

  stop = std::min(stop, size_ - 1);
  size_t suffix_count = size_ - stop - 1;
  while (tail_ != nullptr && suffix_count >= tail_->count) {
    suffix_count -= tail_->count;
    size_ -= tail_->count;
    DeleteNode(tail_);
  }
  if (suffix_count > 0) {
    DeleteFromNode(tail_, ListEnd::kTail, suffix_count);
  }

  size_t prefix_count = start;
  while (head_ != nullptr && prefix_count >= head_->count) {
    prefix_count -= head_->count;
    size_ -= head_->count;
    DeleteNode(head_.get());
  }
  if (prefix_count > 0) {
    DeleteFromNode(head_.get(), ListEnd::kHead, prefix_count);
  }
  // Only the nodes at the two ends shrank.
  MergeAround(head_.get());
  if (tail_ != nullptr) {
    MergeNext(tail_->prev);
  }
}

std::optional<Quicklist::EntryLocation> Quicklist::Locate(size_t index) const {
  if (index >= size_) {
    return std::nullopt;
  }

  if (index < size_ / 2) {
    size_t first_index = 0;
    for (Node* node = head_.get(); node != nullptr; node = node->next.get()) {
      if (index - first_index < node->count) {
        return EntryLocation{node, index - first_index};
      }
      first_index += node->count;
    }
    return std::nullopt;
  }

  size_t end_index = size_;
  for (Node* node = tail_; node != nullptr; node = node->prev) {
    const size_t first_index = end_index - node->count;
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
  return head_ == nullptr ? Listpack::kEmptyListpackBytes
                          : size_t{head_->listpack.TotalBytes()};
}

Listpack Quicklist::ExtractListpack() {
  RS_CHECK(node_count_ <= 1);
  Listpack listpack =
      head_ == nullptr ? Listpack() : std::move(head_->listpack);
  Clear();
  return listpack;
}

void Quicklist::AdoptListpack(Listpack listpack) {
  RS_CHECK(head_ == nullptr);
  const size_t count = listpack.Size();
  if (count == 0) {
    return;
  }
  Node* const node = AppendNode();
  node->listpack = std::move(listpack);
  node->count = count;
  size_ = count;
  NormalizeNodeSizes(node);
}

std::optional<std::string> Quicklist::PopValue(ListEnd end) {
  std::optional<std::string> value;
  Pop(end, [&value](std::string_view element) { value.emplace(element); });
  return value;
}

void Quicklist::MergeAfterPop(Node* node, ListEnd end) {
  if (node->count == 0) {
    DeleteNode(node);
  }
  if (end == ListEnd::kHead) {
    MergeNext(head_.get());
  } else if (tail_ != nullptr) {
    MergeNext(tail_->prev);
  }
}

bool Quicklist::CanAppendToNode(const Node* node,
                                std::string_view value) const {
  const size_t node_bytes = node->listpack.TotalBytes();
  return node_bytes <= node_max_bytes_ &&
         Listpack::EstimateEntryBytes(value) <= node_max_bytes_ - node_bytes;
}

bool Quicklist::CanMergeNodes(const Node* left, const Node* right) const {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  const size_t merged_bytes = size_t{left->listpack.TotalBytes()} +
                              right->listpack.TotalBytes() -
                              Listpack::kEmptyListpackBytes;
  return merged_bytes <= node_max_bytes_;
}

void Quicklist::DeleteFromNode(Node* node, ListEnd end, size_t count) {
  RS_CHECK(node != nullptr && count < node->count);
  const std::optional<size_t> offset =
      node->listpack.OffsetAt(end == ListEnd::kHead ? 0 : node->count - count);
  const size_t deleted =
      offset.has_value() ? node->listpack.DeleteRange(*offset, count) : 0;
  RS_CHECK(deleted == count);
  node->count -= count;
  size_ -= count;
}

void Quicklist::NormalizeNodeSizes(Node* node) {
  // Only `node` changed, and splitting it inserts the pieces before its
  // original successor, so the scan stops there instead of at the tail.
  const Node* const end = node->next.get();
  for (Node* current = node; current != end; current = current->next.get()) {
    while (current->listpack.TotalBytes() > node_max_bytes_ &&
           current->count > 1) {
      SplitNode(current);
    }
  }
}

void Quicklist::SplitNode(Node* node) {
  RS_CHECK(node->count > 1);
  const size_t left_count = node->count / 2;
  auto right = std::make_unique<Node>();
  // Moves the encoded bytes, as Redis' _quicklistSplitNode does, instead of
  // decoding each element and encoding it again.
  right->listpack = node->listpack.SplitOff(left_count);
  right->count = node->count - left_count;
  node->count = left_count;
  InsertNodeAfter(node, std::move(right));
}

void Quicklist::InsertNodeAfter(Node* node, std::unique_ptr<Node> new_node) {
  Node* const inserted = new_node.get();
  new_node->prev = node;
  new_node->next = std::move(node->next);
  if (new_node->next != nullptr) {
    new_node->next->prev = inserted;
  } else {
    tail_ = inserted;
  }
  node->next = std::move(new_node);
  ++node_count_;
}

bool Quicklist::MergeNext(Node* left) {
  Node* const right = left == nullptr ? nullptr : left->next.get();
  // Copies the encoded bytes, as Redis' lpMerge does, instead of decoding
  // each element and encoding it again.
  if (!CanMergeNodes(left, right) ||
      !left->listpack.AppendEntries(right->listpack)) {
    return false;
  }
  left->count += right->count;
  DeleteNode(right);
  return true;
}

void Quicklist::MergeAround(Node* node) {
  if (node == nullptr) {
    return;
  }
  if (node->prev != nullptr && CanMergeNodes(node->prev, node)) {
    node = node->prev;
    MergeNext(node);
  }
  while (MergeNext(node)) {
    // Each pass appends one more node to `node`.
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
