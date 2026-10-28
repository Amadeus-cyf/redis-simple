#ifndef REDIS_SIMPLE_MEMORY_SKIPLIST_H_
#define REDIS_SIMPLE_MEMORY_SKIPLIST_H_

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "utils/check.h"

namespace redis_simple::in_memory {
// Redis skiplists begin with one level and promote at a probability of 0.25.
inline constexpr size_t kInitSkiplistLevel = 1;

// Orders a key against a key, or against another lookup type, with a
// three-way comparison. Any ordering category works (strong, weak, or
// partial); the skiplist only tests results against zero.
template <typename Compare, typename Key, typename Lookup = Key>
concept SkiplistCompareFor =
    std::copy_constructible<Compare> &&
    requires(const Compare& compare, const Key& key, const Lookup& lookup) {
      { compare(key, lookup) } -> std::convertible_to<std::partial_ordering>;
    };

// Sorted set of unique keys with O(log n) insert, delete, rank lookup, and
// range queries, following Redis' zskiplist.
//
// Lookups and range bounds may use any type that Compare can compare a Key
// against, so callers can search without building a full Key: a sorted set
// ordered by (score, member) can search by score alone. Keys live in nodes
// that never move, so references to them stay valid until that key is
// deleted, updated, or the skiplist is cleared.
template <typename Key,
          SkiplistCompareFor<Key> Compare = std::compare_three_way>
  requires std::default_initializable<Key>
class Skiplist {
 public:
  struct Limit {
    size_t offset{};
    // Empty means unbounded.
    std::optional<size_t> count;
  };

  // 0-based ranks in key order; `reverse` counts ranks from the last key and
  // visits keys in descending order.
  struct RankRange {
    size_t min{};
    size_t max{};
    bool minex{};
    bool maxex{};
    std::optional<Limit> limit;
    bool reverse{};
  };

  // Bounds are keys or any lookup type Compare accepts. With `reverse`, keys
  // are visited in descending order, still between `min` and `max`.
  template <typename Bound>
  struct KeyRange {
    Bound min;
    Bound max;
    bool minex{};
    bool maxex{};
    std::optional<Limit> limit;
    bool reverse{};
  };

  class Iterator;

  explicit Skiplist(size_t level = kInitSkiplistLevel, Compare compare = {})
      : head_(std::make_unique<Node>(kMaxLevel)),
        compare_(std::move(compare)),
        level_(ValidateLevel(level)) {}
  Skiplist(const Skiplist&) = delete;
  Skiplist& operator=(const Skiplist&) = delete;
  ~Skiplist() { Clear(); }

  size_t Size() const { return size_; }
  Iterator Begin() const { return Iterator(head_->levels[0].next); }
  Iterator End() const { return Iterator(nullptr); }
  // Iterator at the last key, or End() if empty.
  Iterator Last() const {
    const Node* const node =
        LastWhere([](const Key& /*key*/) { return true; }).node;
    return Iterator(node == head_.get() ? nullptr : node);
  }

  // Inserts `key` unless an equal key exists. Returns the stored key.
  const Key& Insert(Key key) {
    const size_t insert_level = RandomLevel();
    for (size_t i = level_; i < insert_level; ++i) {
      head_->levels[i] = {.next = nullptr, .span = size_};
    }
    level_ = std::max(level_, insert_level);

    Path path;
    Node* const next = FindPath(key, &path);
    if (next != nullptr && compare_(next->key, key) == 0) {
      return next->key;
    }
    auto node = std::make_unique<Node>(insert_level, std::move(key));
    for (size_t i = 0; i < level_; ++i) {
      Level& prev = path.prev[i]->levels[i];
      if (i >= insert_level) {
        ++prev.span;
        continue;
      }
      const size_t distance = path.rank[0] - path.rank[i];
      node->levels[i] = {.next = prev.next, .span = prev.span - distance};
      prev = {.next = node.get(), .span = distance + 1};
    }
    node->prev = path.prev[0] == head_.get() ? nullptr : path.prev[0];
    if (node->levels[0].next != nullptr) {
      node->levels[0].next->prev = node.get();
    }
    ++size_;
    return node.release()->key;
  }

  // Lookups accept a Key, which literals and other convertible values become
  // implicitly, or any type Compare can compare a Key against.
  bool Contains(const Key& key) const { return Contains<Key>(key); }
  template <typename Lookup>
    requires SkiplistCompareFor<Compare, Key, Lookup>
  bool Contains(const Lookup& key) const {
    return Find(key) != nullptr;
  }

  bool Delete(const Key& key) { return Delete<Key>(key); }
  template <typename Lookup>
    requires SkiplistCompareFor<Compare, Key, Lookup>
  bool Delete(const Lookup& key) {
    Path path;
    Node* const node = FindPath(key, &path);
    if (node == nullptr || compare_(node->key, key) != 0) {
      return false;
    }
    Unlink(node, path);
    delete node;
    return true;
  }

  // Applies `update` to the stored key equal to `key`, then moves it if its
  // position changed. The updated key must not equal another stored key.
  // Returns false if no key equals `key`.
  template <std::invocable<Key&> Mutation>
  bool Update(const Key& key, Mutation&& update) {
    return Update<Key>(key, std::forward<Mutation>(update));
  }
  template <typename Lookup, std::invocable<Key&> Mutation>
    requires SkiplistCompareFor<Compare, Key, Lookup>
  bool Update(const Lookup& key, Mutation&& update) {
    Path path;
    Node* const node = FindPath(key, &path);
    if (node == nullptr || compare_(node->key, key) != 0) {
      return false;
    }
    std::invoke(std::forward<Mutation>(update), node->key);
    const Node* const next = node->levels[0].next;
    const bool in_place =
        (node->prev == nullptr || compare_(node->prev->key, node->key) < 0) &&
        (next == nullptr || compare_(node->key, next->key) < 0);
    if (!in_place) {
      Unlink(node, path);
      Key moved = std::move(node->key);
      delete node;
      Insert(std::move(moved));
    }
    return true;
  }

  // Negative ranks count from the end: -1 is the last key.
  const Key& FindKeyByRank(int64_t rank) const {
    if (rank < 0) {
      RS_CHECK(std::cmp_less_equal(size_, std::numeric_limits<int64_t>::max()));
      rank += static_cast<int64_t>(size_);
    }
    RS_CHECK(rank >= 0);
    const Node* const node = NodeAtRank(static_cast<size_t>(rank));
    RS_CHECK(node != nullptr);
    return node->key;
  }

  // Returns the 0-based rank of the key equal to `key`.
  std::optional<size_t> FindRankOfKey(const Key& key) const {
    return FindRankOfKey<Key>(key);
  }
  template <typename Lookup>
    requires SkiplistCompareFor<Compare, Key, Lookup>
  std::optional<size_t> FindRankOfKey(const Lookup& key) const {
    const auto [before, rank] =
        LastWhere([&](const Key& stored) { return compare_(stored, key) < 0; });
    const Node* const next = before->levels[0].next;
    if (next == nullptr || compare_(next->key, key) != 0) {
      return std::nullopt;
    }
    return rank;
  }

  // Calls `visitor(const Key&)` for each key in the rank range.
  template <std::invocable<const Key&> Visitor>
  void ForEachInRange(const RankRange& range, Visitor&& visitor) const {
    const size_t first = range.min + (range.minex ? 1 : 0);
    if (first >= size_ || range.max < first + (range.maxex ? 1 : 0)) {
      return;
    }
    const size_t last = std::min(range.max - (range.maxex ? 1 : 0), size_ - 1);
    size_t remaining = last - first + 1;
    const Node* const start =
        NodeAtRank(range.reverse ? size_ - 1 - first : first);
    Walk(
        start, range.reverse, range.limit,
        [&remaining](const Key&) { return remaining-- > 0; }, visitor);
  }

  // Calls `visitor(const Key&)` for each key in the key range.
  template <typename Bound, std::invocable<const Key&> Visitor>
    requires SkiplistCompareFor<Compare, Key, Bound>
  void ForEachInRange(const KeyRange<Bound>& range, Visitor&& visitor) const {
    if (range.reverse) {
      Walk(
          LastAtOrBelow(range.max, !range.maxex), true, range.limit,
          [&](const Key& key) {
            return AtOrAbove(key, range.min, !range.minex);
          },
          visitor);
    } else {
      Walk(
          FirstAtOrAbove(range.min, !range.minex), false, range.limit,
          [&](const Key& key) {
            return AtOrBelow(key, range.max, !range.maxex);
          },
          visitor);
    }
  }

  // Counts the keys in a key range, ignoring its limit, in O(log n).
  template <typename Bound>
    requires SkiplistCompareFor<Compare, Key, Bound>
  size_t Count(const KeyRange<Bound>& range) const {
    const size_t below = LastWhere([&](const Key& key) {
                           return !AtOrAbove(key, range.min, !range.minex);
                         }).rank;
    const size_t through = LastWhere([&](const Key& key) {
                             return AtOrBelow(key, range.max, !range.maxex);
                           }).rank;
    return through > below ? through - below : 0;
  }

  // Convenience wrappers that copy the matching keys.
  std::vector<Key> RangeByRank(const RankRange& range) const {
    std::vector<Key> keys;
    ForEachInRange(range, [&keys](const Key& key) { keys.push_back(key); });
    return keys;
  }
  template <typename Bound>
  std::vector<Key> RangeByKey(const KeyRange<Bound>& range) const {
    std::vector<Key> keys;
    ForEachInRange(range, [&keys](const Key& key) { keys.push_back(key); });
    return keys;
  }

  void Clear() {
    const Node* node = head_->levels[0].next;
    while (node != nullptr) {
      const Node* const next = node->levels[0].next;
      delete node;
      node = next;
    }
    std::ranges::fill(head_->levels, Level{});
    level_ = kInitSkiplistLevel;
    size_ = 0;
  }

 private:
  static constexpr size_t kMaxLevel = 32;
  static constexpr double kPromoteProbability = 0.25;

  struct Node;

  struct Level {
    Node* next{};
    // Number of level-0 steps from this node to `next`.
    size_t span{};
  };

  struct Node {
    explicit Node(size_t level, Key key = {})
        : key(std::move(key)), levels(level) {}
    Key key;
    // Previous node at level 0; null for the first node.
    Node* prev{};
    std::vector<Level> levels;
  };

  // The last node before the search key at each level, and its rank.
  struct Path {
    std::array<Node*, kMaxLevel> prev{};
    std::array<size_t, kMaxLevel> rank{};
  };

  struct Position {
    const Node* node;
    // 1-based rank of `node`; 0 when `node` is the head.
    size_t rank;
  };

  static size_t ValidateLevel(size_t level) {
    RS_CHECK(level > 0 && level <= kMaxLevel);
    return level;
  }

  size_t RandomLevel() const {
    std::bernoulli_distribution promote(kPromoteProbability);
    size_t level = 1;
    while (level < kMaxLevel && promote(random_generator_)) {
      ++level;
    }
    return level;
  }

  // Walks to the last node whose key satisfies `before`, which must hold for
  // a prefix of the keys.
  template <typename Before>
  Position LastWhere(Before&& before) const {
    const Node* node = head_.get();
    size_t rank = 0;
    for (size_t i = level_; i-- > 0;) {
      while (node->levels[i].next != nullptr &&
             before(node->levels[i].next->key)) {
        rank += node->levels[i].span;
        node = node->levels[i].next;
      }
    }
    return {.node = node, .rank = rank};
  }

  // Records the path to the first key not below `key` and returns that node.
  template <typename Lookup>
  Node* FindPath(const Lookup& key, Path* path) const {
    Node* node = head_.get();
    for (size_t i = level_; i-- > 0;) {
      path->rank[i] = i + 1 == level_ ? 0 : path->rank[i + 1];
      while (node->levels[i].next != nullptr &&
             compare_(node->levels[i].next->key, key) < 0) {
        path->rank[i] += node->levels[i].span;
        node = node->levels[i].next;
      }
      path->prev[i] = node;
    }
    return node->levels[0].next;
  }

  template <typename Lookup>
  const Node* Find(const Lookup& key) const {
    const Node* const next =
        LastWhere([&](const Key& stored) { return compare_(stored, key) < 0; })
            .node->levels[0]
            .next;
    return next != nullptr && compare_(next->key, key) == 0 ? next : nullptr;
  }

  // Removes `node` from every level; the caller owns it afterwards.
  void Unlink(Node* node, const Path& path) {
    for (size_t i = 0; i < level_; ++i) {
      Level& prev = path.prev[i]->levels[i];
      if (prev.next == node) {
        prev.span += node->levels[i].span - 1;
        prev.next = node->levels[i].next;
      } else {
        --prev.span;
      }
    }
    if (node->levels[0].next != nullptr) {
      node->levels[0].next->prev = node->prev;
    }
    --size_;
  }

  // Returns the node at a 0-based rank, or null if out of range.
  const Node* NodeAtRank(size_t rank) const {
    if (rank >= size_) {
      return nullptr;
    }
    const size_t target = rank + 1;
    const Node* node = head_.get();
    size_t traversed = 0;
    for (size_t i = level_; i-- > 0;) {
      while (node->levels[i].next != nullptr &&
             traversed + node->levels[i].span <= target) {
        traversed += node->levels[i].span;
        node = node->levels[i].next;
      }
      if (traversed == target) {
        return node;
      }
    }
    return nullptr;
  }

  template <typename Bound>
  bool AtOrAbove(const Key& key, const Bound& bound, bool inclusive) const {
    const auto order = compare_(key, bound);
    return inclusive ? order >= 0 : order > 0;
  }

  template <typename Bound>
  bool AtOrBelow(const Key& key, const Bound& bound, bool inclusive) const {
    const auto order = compare_(key, bound);
    return inclusive ? order <= 0 : order < 0;
  }

  template <typename Bound>
  const Node* FirstAtOrAbove(const Bound& bound, bool inclusive) const {
    return LastWhere([&](const Key& key) {
             return !AtOrAbove(key, bound, inclusive);
           })
        .node->levels[0]
        .next;
  }

  template <typename Bound>
  const Node* LastAtOrBelow(const Bound& bound, bool inclusive) const {
    const Node* const node = LastWhere([&](const Key& key) {
                               return AtOrBelow(key, bound, inclusive);
                             }).node;
    return node == head_.get() ? nullptr : node;
  }

  // Visits keys from `node` in the given direction while `in_range` holds,
  // applying the limit's offset and count.
  template <typename InRange, typename Visitor>
  static void Walk(const Node* node, bool reverse,
                   const std::optional<Limit>& limit, InRange&& in_range,
                   Visitor& visitor) {
    size_t skip = limit.has_value() ? limit->offset : 0;
    size_t remaining = limit.has_value() && limit->count.has_value()
                           ? *limit->count
                           : std::numeric_limits<size_t>::max();
    for (; node != nullptr && remaining > 0 && in_range(node->key);
         node = reverse ? node->prev : node->levels[0].next) {
      if (skip > 0) {
        --skip;
        continue;
      }
      std::invoke(visitor, node->key);
      --remaining;
    }
  }

  // The head holds no key and has every level, so the list can grow without
  // reallocating it.
  std::unique_ptr<Node> head_;
  Compare compare_;
  size_t level_;
  size_t size_{};
  // Fixed default seed keeps level choices, and so tests, reproducible.
  mutable std::mt19937 random_generator_;
};

template <typename Key, SkiplistCompareFor<Key> Compare>
  requires std::default_initializable<Key>
class Skiplist<Key, Compare>::Iterator {
 public:
  bool Valid() const { return node_ != nullptr; }
  void Next() { node_ = node_->levels[0].next; }
  void Prev() { node_ = node_->prev; }
  Iterator& operator++() {
    Next();
    return *this;
  }
  Iterator& operator--() {
    Prev();
    return *this;
  }
  bool operator==(const Iterator& other) const = default;
  const Key& operator*() const {
    RS_CHECK(node_ != nullptr);
    return node_->key;
  }

 private:
  friend class Skiplist;
  explicit Iterator(const Node* node) : node_(node) {}
  const Node* node_;
};
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_SKIPLIST_H_
