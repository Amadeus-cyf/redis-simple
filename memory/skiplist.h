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

// Orders a stored key against a probe with a three-way comparison. A probe is
// a Key or any other type Compare accepts. Any ordering category works
// (strong, weak, or partial); the skiplist only tests results against zero.
template <typename Compare, typename Key, typename Probe = Key>
concept SkiplistCompareFor =
    std::copy_constructible<Compare> &&
    requires(const Compare& compare, const Key& key, const Probe& probe) {
      { compare(key, probe) } -> std::convertible_to<std::partial_ordering>;
    };

// Sorted set of unique keys with O(log n) insert, delete, rank lookup, and
// range queries, following Redis' zskiplist.
//
// Searches take a probe: a Key, or any type Compare can compare a Key against,
// so callers can search without building a full Key. A probe is used in one of
// two roles:
//  - A lookup (Contains, Delete, Update, FindRankOfKey) must compare equal to
//    at most one stored key, so it carries every field the order uses. A
//    sorted set ordered by (score, member) looks members up by both.
//  - A range bound (KeyRange) marks a position and may compare equal to many
//    keys or none. That sorted set ranges over scores with a bare double.
// Either way, comparing keys against a probe must agree with key order: keys
// below the probe all come before keys equal to it, which come before keys
// above it.
//
// Keys live in nodes that never move, so references to them stay valid until
// that key is deleted, updated, or the skiplist is cleared.
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
    bool min_exclusive{};
    bool max_exclusive{};
    std::optional<Limit> limit;
    bool reverse{};
  };

  // Bounds are probes in the range-bound role. With `reverse`, keys are
  // visited in descending order, still between `min` and `max`.
  template <typename Probe>
  struct KeyRange {
    Probe min;
    Probe max;
    bool min_exclusive{};
    bool max_exclusive{};
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
    return Iterator(LastNodeWhere([](const Key& /*key*/) { return true; }));
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

  // Each lookup has a `const Key&` overload so that literals and other values
  // convertible to Key still work; it forwards to the probe overload.
  bool Contains(const Key& key) const { return Contains<Key>(key); }
  template <typename Probe>
    requires SkiplistCompareFor<Compare, Key, Probe>
  bool Contains(const Probe& lookup) const {
    return Find(lookup) != nullptr;
  }

  bool Delete(const Key& key) { return Delete<Key>(key); }
  template <typename Probe>
    requires SkiplistCompareFor<Compare, Key, Probe>
  bool Delete(const Probe& lookup) {
    Path path;
    Node* const node = FindWithPath(lookup, &path);
    if (node == nullptr) {
      return false;
    }
    Unlink(node, path);
    delete node;
    return true;
  }

  // Applies `mutation` to the stored key equal to `lookup`, then moves the key
  // if its position changed. The changed key must not equal another stored
  // key. Returns false if no key equals `lookup`.
  template <std::invocable<Key&> Mutation>
  bool Update(const Key& key, Mutation&& mutation) {
    return Update<Key>(key, std::forward<Mutation>(mutation));
  }
  template <typename Probe, std::invocable<Key&> Mutation>
    requires SkiplistCompareFor<Compare, Key, Probe>
  bool Update(const Probe& lookup, Mutation&& mutation) {
    Path path;
    Node* const node = FindWithPath(lookup, &path);
    if (node == nullptr) {
      return false;
    }
    std::invoke(std::forward<Mutation>(mutation), node->key);
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

  // Returns the 0-based rank of the key equal to `lookup`.
  std::optional<size_t> FindRankOfKey(const Key& key) const {
    return FindRankOfKey<Key>(key);
  }
  template <typename Probe>
    requires SkiplistCompareFor<Compare, Key, Probe>
  std::optional<size_t> FindRankOfKey(const Probe& lookup) const {
    const auto [before, rank] = LastWhere(
        [&](const Key& stored) { return compare_(stored, lookup) < 0; });
    const Node* const next = before->levels[0].next;
    if (next == nullptr || compare_(next->key, lookup) != 0) {
      return std::nullopt;
    }
    return rank;
  }

  // Calls `visitor(const Key&)` for each key in the rank range.
  template <std::invocable<const Key&> Visitor>
  void ForEachInRange(const RankRange& range, Visitor&& visitor) const {
    const size_t first = range.min + (range.min_exclusive ? 1 : 0);
    if (first >= size_ || range.max < first + (range.max_exclusive ? 1 : 0)) {
      return;
    }
    const size_t last =
        std::min(range.max - (range.max_exclusive ? 1 : 0), size_ - 1);
    size_t remaining = last - first + 1;
    const Node* const start =
        NodeAtRank(range.reverse ? size_ - 1 - first : first);
    Walk(
        start, range.reverse, range.limit,
        [&remaining](const Key&) { return remaining-- > 0; }, visitor);
  }

  // Calls `visitor(const Key&)` for each key in the key range.
  template <typename Probe, std::invocable<const Key&> Visitor>
    requires SkiplistCompareFor<Compare, Key, Probe>
  void ForEachInRange(const KeyRange<Probe>& range, Visitor&& visitor) const {
    const auto below_min = BelowMin(range);
    const auto within_max = WithinMax(range);
    if (range.reverse) {
      Walk(
          LastNodeWhere(within_max), /*reverse=*/true, range.limit,
          [&](const Key& key) { return !below_min(key); }, visitor);
    } else {
      Walk(FirstNodeWhereNot(below_min), /*reverse=*/false, range.limit,
           within_max, visitor);
    }
  }

  // Counts the keys in a key range, ignoring its limit, in O(log n).
  template <typename Probe>
    requires SkiplistCompareFor<Compare, Key, Probe>
  size_t Count(const KeyRange<Probe>& range) const {
    const size_t below_min = LastWhere(BelowMin(range)).rank;
    const size_t through_max = LastWhere(WithinMax(range)).rank;
    return through_max > below_min ? through_max - below_min : 0;
  }

  // Convenience wrappers that copy the matching keys.
  std::vector<Key> RangeByRank(const RankRange& range) const {
    std::vector<Key> keys;
    ForEachInRange(range, [&keys](const Key& key) { keys.push_back(key); });
    return keys;
  }
  template <typename Probe>
    requires SkiplistCompareFor<Compare, Key, Probe>
  std::vector<Key> RangeByKey(const KeyRange<Probe>& range) const {
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

  // Search primitives. `before` must hold for a prefix of the keys in order
  // and fail for the rest, which is what probe ordering guarantees.

  // Walks to the last node whose key satisfies `before`; the head if none.
  template <std::predicate<const Key&> Before>
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

  // The last node satisfying `before`, or null if none does.
  template <std::predicate<const Key&> Before>
  const Node* LastNodeWhere(Before&& before) const {
    const Node* const node = LastWhere(std::forward<Before>(before)).node;
    return node == head_.get() ? nullptr : node;
  }

  // The first node not satisfying `before`, or null if every node does.
  template <std::predicate<const Key&> Before>
  const Node* FirstNodeWhereNot(Before&& before) const {
    return LastWhere(std::forward<Before>(before)).node->levels[0].next;
  }

  // Like LastWhere for keys below `probe`, but also records the path for
  // relinking. Returns the first node not below `probe`.
  template <typename Probe>
  Node* FindPath(const Probe& probe, Path* path) const {
    Node* node = head_.get();
    for (size_t i = level_; i-- > 0;) {
      path->rank[i] = i + 1 == level_ ? 0 : path->rank[i + 1];
      while (node->levels[i].next != nullptr &&
             compare_(node->levels[i].next->key, probe) < 0) {
        path->rank[i] += node->levels[i].span;
        node = node->levels[i].next;
      }
      path->prev[i] = node;
    }
    return node->levels[0].next;
  }

  // The node equal to `lookup` with its path recorded, or null.
  template <typename Probe>
  Node* FindWithPath(const Probe& lookup, Path* path) const {
    Node* const node = FindPath(lookup, path);
    return node != nullptr && compare_(node->key, lookup) == 0 ? node : nullptr;
  }

  // The node equal to `lookup`, or null.
  template <typename Probe>
  const Node* Find(const Probe& lookup) const {
    const Node* const node = FirstNodeWhereNot(
        [&](const Key& key) { return compare_(key, lookup) < 0; });
    return node != nullptr && compare_(node->key, lookup) == 0 ? node : nullptr;
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

  // Whether a key falls before the range's lower bound; holds for a prefix.
  template <typename Probe>
  auto BelowMin(const KeyRange<Probe>& range) const {
    return [this, &range](const Key& key) {
      const auto order = compare_(key, range.min);
      return !(range.min_exclusive ? order > 0 : order >= 0);
    };
  }

  // Whether a key is at or before the range's upper bound; holds for a prefix.
  template <typename Probe>
  auto WithinMax(const KeyRange<Probe>& range) const {
    return [this, &range](const Key& key) {
      const auto order = compare_(key, range.max);
      return range.max_exclusive ? order < 0 : order <= 0;
    };
  }

  // Visits keys from `node` in the given direction while `in_range` holds,
  // applying the limit's offset and count.
  template <std::predicate<const Key&> InRange,
            std::invocable<const Key&> Visitor>
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
