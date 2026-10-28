#ifndef REDIS_SIMPLE_MEMORY_SKIPLIST_H_
#define REDIS_SIMPLE_MEMORY_SKIPLIST_H_

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "utils/check.h"

namespace redis_simple::in_memory {
// Redis skiplists begin with one level and promote at a probability of 0.25.
inline constexpr size_t kInitSkiplistLevel = 1;

// Default comparator
template <typename Key>
const auto kDefaultCompare = [](const Key& k1, const Key& k2) {
  return k1 < k2 ? -1 : (k1 == k2 ? 0 : 1);
};

// Default destructor
template <typename Key>
const auto kDefaultDestructor = [](const Key&) {};

// A Skiplist comparator orders two keys like strcmp: negative, zero, or
// positive.
template <typename Comparator, typename Key>
concept SkiplistComparatorFor =
    std::copy_constructible<Comparator> &&
    requires(const Comparator& compare, const Key& left, const Key& right) {
      { compare(left, right) } -> std::convertible_to<int>;
    };

// A Skiplist destructor releases whatever a key owns when its node is freed.
template <typename Destructor, typename Key>
concept SkiplistDestructorFor = std::copy_constructible<Destructor> &&
                                std::invocable<const Destructor&, const Key&>;

template <typename Key, typename Comparator = decltype(kDefaultCompare<Key>),
          typename Destructor = decltype(kDefaultDestructor<Key>)>
class Skiplist {
  // Checked here rather than on the template parameters so the out-of-line
  // member definitions keep their short template heads.
  static_assert(SkiplistComparatorFor<Comparator, Key>,
                "Comparator must order two const Key& keys as an int");
  static_assert(SkiplistDestructorFor<Destructor, Key>,
                "Destructor must accept a const Key&");

 private:
  struct SkiplistLevel;
  class SkiplistNode;

 public:
  // Spec for LIMIT flag
  struct SkiplistLimitSpec {
    SkiplistLimitSpec() : offset(0), count(std::nullopt) {}
    SkiplistLimitSpec(size_t offset, std::optional<size_t> count)
        : offset(offset), count(count) {}
    // Starting offset
    size_t offset;
    // Number of keys to return; empty means unbounded.
    std::optional<size_t> count;
  };
  // Spec for range by rank
  struct SkiplistRangeByRankSpec {
    SkiplistRangeByRankSpec()
        : min(0), max(0), minex(false), maxex(false), limit(nullptr) {}
    SkiplistRangeByRankSpec(size_t min, size_t max, bool minex, bool maxex,
                            const SkiplistLimitSpec* limit)
        : min(min), max(max), minex(minex), maxex(maxex), limit(limit) {}
    // Min and max index
    size_t min, max;
    // Are min or max exclusive?
    bool minex, maxex;
    // Set if LIMIT flag is included
    const SkiplistLimitSpec* limit;
  };
  // Spec for range by key
  struct SkiplistRangeByKeySpec {
    SkiplistRangeByKeySpec(const Key& min, bool minex, const Key& max,
                           bool maxex, const SkiplistLimitSpec* limit)
        : min(min), max(max), minex(minex), maxex(maxex), limit(limit) {}
    // Min and max key
    const Key min, max;
    // Min or max exclusive?
    bool minex, maxex;
    // Set if LIMIT flag is included
    const SkiplistLimitSpec* limit;
  };
  // Skiplist iterator
  class Iterator;
  Skiplist();
  explicit Skiplist(size_t level);
  explicit Skiplist(size_t level, const Comparator& compare);
  explicit Skiplist(size_t level, const Comparator& compare,
                    const Destructor& dtr);
  Skiplist(const Skiplist&) = delete;
  Skiplist& operator=(const Skiplist&) = delete;
  Iterator Begin() const;
  Iterator End() const;
  const Key& Insert(const Key& key);
  bool Contains(const Key& key) const;
  bool Delete(const Key& key);
  bool Update(const Key& key, const Key& new_key);
  const Key& FindKeyByRank(int64_t rank) const;
  std::optional<size_t> FindRankOfKey(const Key& key) const;
  std::vector<Key> RangeByRank(const SkiplistRangeByRankSpec* spec) const;
  std::vector<Key> RevRangeByRank(const SkiplistRangeByRankSpec* spec) const;
  std::vector<Key> RangeByKey(const SkiplistRangeByKeySpec* spec) const;
  std::vector<Key> RevRangeByKey(const SkiplistRangeByKeySpec* spec) const;
  size_t Count(const SkiplistRangeByKeySpec* spec) const;
  const Key& operator[](size_t i);
  size_t Size() const { return size_; }
  void Clear();
  void Print() const;
  ~Skiplist();

 private:
  static constexpr size_t kMaxSkiplistLevel = 32;
  static constexpr double kSkiplistP = 0.25;
  static size_t ValidateLevel(size_t level) {
    RS_CHECK(level > 0 && level <= kMaxSkiplistLevel);
    return level;
  }
  size_t RandomLevel() const;
  bool Lt(const Key& k1, const Key& k2) const;
  bool Lte(const Key& k1, const Key& k2) const;
  bool Gt(const Key& k1, const Key& k2) const;
  bool Gte(const Key& k1, const Key& k2) const;
  bool Eq(const Key& k1, const Key& k2) const;
  void DeleteNode(const Key& key, SkiplistNode** const prev);
  const SkiplistNode* FindKey(size_t rank) const;
  const SkiplistNode* FindMinNodeByRangeRankSpec(
      const SkiplistRangeByRankSpec* spec) const;
  const SkiplistNode* FindRevMinNodeByRangeRankSpec(
      const SkiplistRangeByRankSpec* spec) const;
  bool ValidateRangeRankSpec(const SkiplistRangeByRankSpec* spec) const;
  bool ValidateRangeKeySpec(const SkiplistRangeByKeySpec* spec) const;
  std::vector<Key> RangeByRankWithValidSpec(
      const SkiplistRangeByRankSpec* spec) const;
  std::vector<Key> RevRangeByRankWithValidSpec(
      const SkiplistRangeByRankSpec* spec) const;
  std::vector<Key> RangeByKeyWithValidSpec(
      const SkiplistRangeByKeySpec* spec) const;
  std::vector<Key> RevRangeByKeyWithValidSpec(
      const SkiplistRangeByKeySpec* spec) const;
  size_t CountWithValidSpec(const SkiplistRangeByKeySpec* spec) const;
  SkiplistNode* FindKeyGreaterOrEqual(const Key& key, SkiplistNode** const prev,
                                      size_t* const rank) const;
  const SkiplistNode* FindKeyGreaterOrEqual(const Key& key) const;
  const SkiplistNode* FindKeyGreaterThan(const Key& key) const;
  const SkiplistNode* FindKeyLessOrEqual(const Key& key) const;
  const SkiplistNode* FindKeyLessThan(const Key& key) const;
  const SkiplistNode* FindKeyLess(const Key& key, bool eq) const;
  const SkiplistNode* FindLast() const;
  void Reset();
  // Skiplist node head
  SkiplistNode* head_;
  // Key comparator
  const Comparator compare_;
  // Key destructor
  const Destructor dtr_;
  // Number of levels
  size_t level_;
  // Number of nodes
  size_t size_;
  // Deterministic RNG keeps tests reproducible while avoiding C rand().
  mutable std::mt19937 random_generator_;
};

// SkiplistLevel
template <typename Key, typename Comparator, typename Destructor>
struct Skiplist<Key, Comparator, Destructor>::SkiplistLevel {
  SkiplistNode* next_{nullptr};
  size_t span_{};
};

// SkiplistNode
template <typename Key, typename Comparator, typename Destructor>
class Skiplist<Key, Comparator, Destructor>::SkiplistNode {
 public:
  static SkiplistNode* Create(const Key& key, size_t level,
                              const Destructor& dtr);
  static SkiplistNode* Create(size_t level, const Destructor& dtr);
  const SkiplistNode* Next(size_t level) const { return levels_[level].next_; }
  SkiplistNode* Next(size_t level) { return levels_[level].next_; }
  void SetNext(size_t level, SkiplistNode* next) {
    levels_[level].next_ = next;
  }
  size_t Span(size_t level) const { return levels_[level].span_; }
  void SetSpan(size_t level, size_t span) { levels_[level].span_ = span; }
  const SkiplistNode* Prev() const { return prev_; }
  SkiplistNode* Prev() { return prev_; }
  void SetPrev(SkiplistNode* prev) { prev_ = prev; }
  void InitLevel(size_t level) {
    if (level >= levels_.size()) {
      levels_.resize(level + 1);
    }
  }
  void Reset();
  ~SkiplistNode();
  Key key;

 private:
  SkiplistNode(size_t level, const Destructor& dtr)
      : key{}, levels_(level), prev_(nullptr), dtr_(dtr), owns_key_(false) {}
  SkiplistNode(const Key& key, size_t level, Destructor dtr)
      : key(key),
        levels_(level),
        prev_(nullptr),
        dtr_(std::move(dtr)),
        owns_key_(true) {}
  std::vector<SkiplistLevel> levels_;
  SkiplistNode* prev_;
  const Destructor dtr_;
  const bool owns_key_;
};

template <typename Key, typename Comparator, typename Destructor>
typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::SkiplistNode::Create(
    const Key& key, size_t level, const Destructor& dtr) {
  return new SkiplistNode(key, level, dtr);
}

template <typename Key, typename Comparator, typename Destructor>
typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::SkiplistNode::Create(
    size_t level, const Destructor& dtr) {
  return new SkiplistNode(level, dtr);
}

template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::SkiplistNode::Reset() {
  std::fill(levels_.begin(), levels_.end(), SkiplistLevel{});
  prev_ = nullptr;
}

template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::SkiplistNode::~SkiplistNode() {
  Reset();
  if (owns_key_) {
    dtr_(key);
  }
}

// Iterator
template <typename Key, typename Comparator, typename Destructor>
class Skiplist<Key, Comparator, Destructor>::Iterator {
 public:
  explicit Iterator(const Skiplist* skiplist);
  explicit Iterator(const Skiplist* skiplist, const SkiplistNode* node);
  Iterator(const Iterator& it) = default;
  // Return true if the iterator is positioned to a valid node.
  bool Valid() const;
  // Position at the first node in the list.
  void SeekToFirst();
  // Position at the last node in the list.
  void SeekToLast();
  // Advance to the next node in the list.
  void Next();
  // Advance to the previous node in the list.
  void Prev();
  Iterator& operator=(const Iterator& it) = default;
  // Move to the next node in the list.
  Iterator& operator--();
  // Move to the previous node in the list.
  Iterator& operator++();
  bool operator==(const Iterator& it) const = default;
  const Key& operator*() const;

 private:
  const SkiplistNode* node_;
  const Skiplist* skiplist_;
};

template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::Iterator::Iterator(
    const Skiplist* skiplist)
    : node_(nullptr), skiplist_(skiplist) {}

template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::Iterator::Iterator(
    const Skiplist* skiplist, const SkiplistNode* node)
    : node_(node), skiplist_(skiplist) {}

template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Iterator::Valid() const {
  return node_ != nullptr;
}

template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::Iterator::SeekToFirst() {
  node_ = skiplist_->head_->Next(0);
}

template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::Iterator::SeekToLast() {
  node_ = skiplist_->FindLast();
  if (node_ == skiplist_->head_) node_ = nullptr;
}

template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::Iterator::Next() {
  node_ = node_->Next(0);
}

template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::Iterator::Prev() {
  node_ = node_->Prev();
}

template <typename Key, typename Comparator, typename Destructor>
typename Skiplist<Key, Comparator, Destructor>::Iterator&
Skiplist<Key, Comparator, Destructor>::Iterator::operator++() {
  node_ = node_->Next(0);
  return *this;
}

template <typename Key, typename Comparator, typename Destructor>
typename Skiplist<Key, Comparator, Destructor>::Iterator&
Skiplist<Key, Comparator, Destructor>::Iterator::operator--() {
  node_ = node_->Prev();
  return *this;
}

template <typename Key, typename Comparator, typename Destructor>
const Key& Skiplist<Key, Comparator, Destructor>::Iterator::operator*() const {
  RS_CHECK(node_ != nullptr);
  return node_->key;
}

// Skiplist
template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::Skiplist()
    : head_(SkiplistNode::Create(kInitSkiplistLevel, kDefaultDestructor<Key>)),
      compare_(kDefaultCompare<Key>),
      dtr_(kDefaultDestructor<Key>),
      level_(kInitSkiplistLevel),
      size_(0),
      random_generator_(std::mt19937::default_seed) {}

template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::Skiplist(size_t level)
    : head_(
          SkiplistNode::Create(ValidateLevel(level), kDefaultDestructor<Key>)),
      compare_(kDefaultCompare<Key>),
      dtr_(kDefaultDestructor<Key>),
      level_(level),
      size_(0),
      random_generator_(std::mt19937::default_seed) {}

template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::Skiplist(size_t level,
                                                const Comparator& compare)
    : head_(
          SkiplistNode::Create(ValidateLevel(level), kDefaultDestructor<Key>)),
      compare_(compare),
      dtr_(kDefaultDestructor<Key>),
      level_(level),
      size_(0),
      random_generator_(std::mt19937::default_seed) {}

template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::Skiplist(size_t level,
                                                const Comparator& compare,
                                                const Destructor& dtr)
    : head_(SkiplistNode::Create(ValidateLevel(level), dtr)),
      compare_(compare),
      dtr_(dtr),
      level_(level),
      size_(0),
      random_generator_(std::mt19937::default_seed) {}

/*
 * Return an iterator pointing to the first node.
 */
template <typename Key, typename Comparator, typename Destructor>
typename Skiplist<Key, Comparator, Destructor>::Iterator
Skiplist<Key, Comparator, Destructor>::Begin() const {
  return Iterator(this, head_->Next(0));
}

/*
 * Return an iterator pointing to the last  node.
 */
template <typename Key, typename Comparator, typename Destructor>
typename Skiplist<Key, Comparator, Destructor>::Iterator
Skiplist<Key, Comparator, Destructor>::End() const {
  return Iterator(this, nullptr);
}

/*
 * Return a randomnized level used for insertion.
 */
template <typename Key, typename Comparator, typename Destructor>
size_t Skiplist<Key, Comparator, Destructor>::RandomLevel() const {
  size_t level = 1;
  std::bernoulli_distribution promote(kSkiplistP);
  while (level < kMaxSkiplistLevel && promote(random_generator_)) {
    ++level;
  }
  return level;
}

template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Lt(const Key& k1,
                                               const Key& k2) const {
  return compare_(k1, k2) < 0;
}

template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Lte(const Key& k1,
                                                const Key& k2) const {
  return Lt(k1, k2) || Eq(k1, k2);
}

template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Gt(const Key& k1,
                                               const Key& k2) const {
  return compare_(k1, k2) > 0;
}

template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Gte(const Key& k1,
                                                const Key& k2) const {
  return Gt(k1, k2) || Eq(k1, k2);
}

template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Eq(const Key& k1,
                                               const Key& k2) const {
  return compare_(k1, k2) == 0;
}

/*
 * Insert a new key into the skiplist. If the key already exists, return the
 * node containing the key and skip the insertion. Otherwise, create a new node
 * with the given key and insert it into the skiplist. Return the newly created
 * node as the result.
 */
template <typename Key, typename Comparator, typename Destructor>
const Key& Skiplist<Key, Comparator, Destructor>::Insert(const Key& key) {
  RS_CHECK(level_ != 0);
  const size_t insert_level = RandomLevel();
  // If the random level is larger than level, init empty skiplist level for
  // extra levels and update level to the random level.
  for (size_t i = level_; i < insert_level; ++i) {
    head_->InitLevel(i);
    head_->SetSpan(i, size_);
  }
  if (insert_level > level_) {
    level_ = insert_level;
  }
  // Find previous key at each level having the next key greater than or equal
  // to the given key.
  std::array<SkiplistNode*, kMaxSkiplistLevel> prev{};
  std::array<size_t, kMaxSkiplistLevel> rank{};
  const SkiplistNode* n = FindKeyGreaterOrEqual(key, prev.data(), rank.data());
  if (n && Eq(n->key, key)) {
    // If key already exists, do not insert,
    return n->key;
  }
  // Insert the key and update span.
  std::unique_ptr<SkiplistNode> node(SkiplistNode::Create(key, level_, dtr_));
  SkiplistNode* node_ptr = node.get();
  for (size_t i = 0; i < level_; ++i) {
    if (i < insert_level) {
      // Need to insert the key
      size_t span = prev[i]->Span(i);
      node_ptr->SetNext(i, prev[i]->Next(i));
      node_ptr->SetSpan(i, span - rank[0] + rank[i]);
      prev[i]->SetNext(i, node_ptr);
      prev[i]->SetSpan(i, rank[0] - rank[i] + 1);
    } else {
      // No need to insert key, only increase span by 1.
      prev[i]->SetSpan(i, prev[i]->Span(i) + 1);
    }
  }
  // For level 0, update backward pointer since it's a double linked list.
  node_ptr->SetPrev(prev[0]);
  if (node_ptr->Next(0)) {
    node_ptr->Next(0)->SetPrev(node_ptr);
  }
  ++size_;
  const Key& inserted_key = node_ptr->key;
  node.release();
  return inserted_key;
}

/*
 * Return true if the node containing the key is found in the skiplist.
 */
template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Contains(const Key& key) const {
  const SkiplistNode* n = FindKeyGreaterOrEqual(key);
  return n && Eq(n->key, key);
}

/*
 * Delete the given node containing the key from the skiplist. Return true if
 * the key is deleted, and false if the key is not found.
 */
template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Delete(const Key& key) {
  std::array<SkiplistNode*, kMaxSkiplistLevel> prev{};
  const SkiplistNode* n = FindKeyGreaterOrEqual(key, prev.data(), nullptr);
  if (!n || !Eq(n->key, key)) {
    return false;
  }
  DeleteNode(key, prev.data());
  return true;
}

/*
 * Update the key. Return true if the key is updated, and false if the original
 * key is not found.
 */
template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::Update(const Key& key,
                                                   const Key& new_key) {
  std::array<SkiplistNode*, kMaxSkiplistLevel> prev{};
  const SkiplistNode* n = FindKeyGreaterOrEqual(key, prev.data(), nullptr);
  if (!n || !Eq(n->key, key)) {
    // Key not found.
    return false;
  }
  const SkiplistNode* next = n->Next(0);
  if ((prev[0] == head_ || Gte(new_key, prev[0]->key)) &&
      (!next || Lte(new_key, next->key))) {
    // If in the key's position is not changed, update the key directly.
    SkiplistNode* next = prev[0]->Next(0);
    // Delete the old key.
    dtr_(next->key);
    next->key = new_key;
  } else {
    // Otherwise, delete the original node and insert a new one.
    DeleteNode(key, prev.data());
    Insert(new_key);
  }
  return true;
}

/*
 * Return the key at the given index.
 */
template <typename Key, typename Comparator, typename Destructor>
const Key& Skiplist<Key, Comparator, Destructor>::FindKeyByRank(
    int64_t rank) const {
  if (rank < 0) {
    RS_CHECK(size_ <= static_cast<size_t>(std::numeric_limits<int64_t>::max()));
    rank += static_cast<int64_t>(size_);
  }
  RS_CHECK(rank >= 0);
  const SkiplistNode* node = FindKey(static_cast<size_t>(rank));
  RS_CHECK(node != nullptr);
  return node->key;
}

/*
 * Return the index of the key.
 */
template <typename Key, typename Comparator, typename Destructor>
std::optional<size_t> Skiplist<Key, Comparator, Destructor>::FindRankOfKey(
    const Key& key) const {
  size_t rank = 0;
  const SkiplistNode* node = head_;
  for (size_t i = level_; i-- > 0;) {
    while (node->Next(i) && Lt(node->Next(i)->key, key)) {
      rank += node->Span(i);
      node = node->Next(i);
    }
    if (node->Next(i) && Eq(node->Next(i)->key, key)) {
      return rank + node->Span(i) - 1;
    }
  }
  return std::nullopt;
}

/*
 * Return all keys within the rank range.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key> Skiplist<Key, Comparator, Destructor>::RangeByRank(
    const SkiplistRangeByRankSpec* spec) const {
  return ValidateRangeRankSpec(spec) ? RangeByRankWithValidSpec(spec)
                                     : std::vector<Key>();
}

/*
 * Return the keys which have the reversed rank within the range.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key> Skiplist<Key, Comparator, Destructor>::RevRangeByRank(
    const SkiplistRangeByRankSpec* spec) const {
  return ValidateRangeRankSpec(spec) ? RevRangeByRankWithValidSpec(spec)
                                     : std::vector<Key>();
}

/*
 * Return all keys within the range in ascending order.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key> Skiplist<Key, Comparator, Destructor>::RangeByKey(
    const SkiplistRangeByKeySpec* spec) const {
  return ValidateRangeKeySpec(spec) ? RangeByKeyWithValidSpec(spec)
                                    : std::vector<Key>();
}

/*
 * Return all keys within the range in descending order.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key> Skiplist<Key, Comparator, Destructor>::RevRangeByKey(
    const SkiplistRangeByKeySpec* spec) const {
  return ValidateRangeKeySpec(spec) ? RevRangeByKeyWithValidSpec(spec)
                                    : std::vector<Key>();
}

/*
 * Return the count of keys which have the rank within range.
 */
template <typename Key, typename Comparator, typename Destructor>
size_t Skiplist<Key, Comparator, Destructor>::Count(
    const SkiplistRangeByKeySpec* spec) const {
  return ValidateRangeKeySpec(spec) ? CountWithValidSpec(spec) : 0;
}

/*
 * Return the first key greater than or equal to the given key.
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindKeyGreaterOrEqual(
    const Key& key) const {
  const SkiplistNode* n = FindKeyGreaterOrEqual(key, nullptr, nullptr);
  return n;
}

/*
 * Return the first key greater than the given key.
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindKeyGreaterThan(
    const Key& key) const {
  const SkiplistNode* n = FindKeyGreaterOrEqual(key);
  return (n && Eq(n->key, key)) ? n->Next(0) : n;
}

/*
 * Return the last key less than or equal to the given key.
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindKeyLessOrEqual(
    const Key& key) const {
  return FindKeyLess(key, true);
}

/*
 * Return the last key less than the given key.
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindKeyLessThan(const Key& key) const {
  return FindKeyLess(key, false);
}

template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindKeyLess(const Key& key,
                                                   bool eq) const {
  const SkiplistNode* node = head_;
  for (size_t i = level_; i-- > 0;) {
    while (node->Next(i) &&
           (eq ? Lte(node->Next(i)->key, key) : Lt(node->Next(i)->key, key))) {
      node = node->Next(i);
    }
  }
  return node;
}

template <typename Key, typename Comparator, typename Destructor>
const Key& Skiplist<Key, Comparator, Destructor>::operator[](size_t i) {
  const SkiplistNode* node = FindKey(i);
  RS_CHECK(node != nullptr);
  return node->key;
}

/*
 * Remove all nodes from the skiplist.
 */
template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::Clear() {
  Reset();
  level_ = kInitSkiplistLevel;
}

template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::Print() const {
  const SkiplistNode* node = head_;
  for (size_t i = level_; i-- > 0;) {
    RS_LOG_DEBUG("h%zu", i);
    while (node) {
      std::cout << node->key;
      RS_LOG_DEBUG("---%zu---", node->Span(i));
      node = node->Next(i);
    }
    RS_LOG_DEBUG("end\n");
    node = head_;
  }
}

/*
 * Find the first key greater than or equal to the given key.
 * If prev is non-null, fills prev[level] with pointer to previous node at
 * "level" for every level in [0...level_-1].
 * If rank is non-null, fills rank[level] with the rank of the previous key at
 * "level" for every level in [0...level_-1];
 */
template <typename Key, typename Comparator, typename Destructor>
typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindKeyGreaterOrEqual(
    const Key& key, SkiplistNode** const prev, size_t* const rank) const {
  SkiplistNode* n = head_;
  for (size_t i = level_; i-- > 0;) {
    if (rank) {
      rank[i] = (i == level_ - 1) ? 0 : rank[i + 1];
    }
    while (n->Next(i) && Lt(n->Next(i)->key, key)) {
      if (rank) {
        rank[i] += n->Span(i);
      }
      n = n->Next(i);
    }
    if (prev) {
      prev[i] = n;
    }
    if (i == 0 && n) {
      return n->Next(0);
    }
  }
  return nullptr;
}

/*
 * Delete the node from the skiplist.
 * The function assumes that the node exists in the skiplist.
 */
template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::DeleteNode(
    const Key& key, SkiplistNode** const prev) {
  SkiplistNode* node_to_delete = prev[0]->Next(0);
  for (size_t i = level_; i-- > 0;) {
    SkiplistNode* next = prev[i]->Next(i);
    if (next && Eq(next->key, key)) {
      prev[i]->SetNext(i, next ? next->Next(i) : nullptr);
      prev[i]->SetSpan(i, prev[i]->Span(i) + (next ? next->Span(i) : 0) - 1);
    } else {
      prev[i]->SetSpan(i, prev[i]->Span(i) - 1);
    }
  }
  // For level 0, update backward pointer since it's a double linked list.
  if (prev[0]->Next(0)) {
    prev[0]->Next(0)->SetPrev(prev[0]);
  }
  delete node_to_delete;
  --size_;
}

/*
 * Return the key at the given index. Nullptr if index is out of range.
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindKey(size_t rank) const {
  if (rank >= size_) {
    return nullptr;
  }
  size_t span = 0;
  const SkiplistNode* node = head_;
  for (size_t i = level_; i-- > 0;) {
    while (node->Next(i) && (span + node->Span(i) < rank + 1)) {
      span += node->Span(i);
      node = node->Next(i);
    }

    if (node->Next(i) && span + node->Span(i) == rank + 1) {
      return node->Next(i);
    }
  }
  return nullptr;
}

/*
 * Return the first node having rank in the rank range. If spec has minex =
 * true, the returned node should be the one next to the node at the min rank.
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindMinNodeByRangeRankSpec(
    const SkiplistRangeByRankSpec* spec) const {
  const SkiplistNode* node = FindKey(spec->min);
  // If min rank is excluded, start at the next node.
  if (node && spec->minex) {
    node = node->Next(0);
  }
  return node;
}

/*
 * Return the first node having reversed rank in the rank range. If spec has
 * minex = true, the returned node should be the one previous to the node at the
 * min rank.
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindRevMinNodeByRangeRankSpec(
    const SkiplistRangeByRankSpec* spec) const {
  const SkiplistNode* node = FindKey(size_ - 1 - spec->min);
  // If min rank is excluded, start at the previous node.
  if (node && spec->minex) {
    node = node->Prev();
  }
  return node;
}

/*
 * Validate the range rank spec. Return true if the spec is
 * valid.
 */
template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::ValidateRangeRankSpec(
    const SkiplistRangeByRankSpec* spec) const {
  return spec && spec->min >= 0 && spec->max >= 0 &&
         ((!spec->minex && !spec->maxex && spec->min <= spec->max) ||
          spec->min < spec->max);
}

/*
 * Validate the range key spec. Return true if the spec is
 * valid.
 */
template <typename Key, typename Comparator, typename Destructor>
bool Skiplist<Key, Comparator, Destructor>::ValidateRangeKeySpec(
    const SkiplistRangeByKeySpec* spec) const {
  return spec && ((!spec->minex && !spec->maxex && Lte(spec->min, spec->max)) ||
                  Lt(spec->min, spec->max));
}

/*
 * Find keys in reverse range of rank. Rank indicates the position of the key in
 * the skiplist.
 * The function assumes the spec are valid. Should call ValidateRangeRankSpec
 * before calling this function.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key>
Skiplist<Key, Comparator, Destructor>::RangeByRankWithValidSpec(
    const SkiplistRangeByRankSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) return {};
  const SkiplistNode* node = FindMinNodeByRangeRankSpec(spec);
  if (!node) {
    return {};
  }
  std::vector<Key> keys;
  size_t i = 0, start = spec->min + (spec->minex ? 1 : 0),
         end = spec->max + (spec->maxex ? -1 : 0),
         offset = spec->limit ? spec->limit->offset : 0;
  if (count == 0) return {};
  while (node && start <= end) {
    ++start;
    // Add the key if the current rank is larger of equal to the specified
    // offset.
    if (i++ >= offset) {
      keys.push_back(node->key);
      // Return the keys if the number reach the limit, only works when the
      // limit is a non-negative value.
      if (count.has_value() && keys.size() >= *count) {
        return keys;
      }
    }
    node = node->Next(0);
  }
  return keys;
}

/*
 * Find keys in reverse range of rank. Rank indicates the position of the key in
 * the skiplist.
 * The function assumes the spec are valid with non-negative min and max value.
 * Should call ValidateRangeRankSpec before calling this function.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key>
Skiplist<Key, Comparator, Destructor>::RevRangeByRankWithValidSpec(
    const SkiplistRangeByRankSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) return {};
  const SkiplistNode* node = FindRevMinNodeByRangeRankSpec(spec);
  if (!node) {
    return {};
  }
  std::vector<Key> keys;
  size_t i = 0, start = spec->min + (spec->minex ? 1 : 0),
         end = spec->max + (spec->maxex ? -1 : 0),
         offset = spec->limit ? spec->limit->offset : 0;
  while (node && start <= end) {
    // Return the keys if the number reach the limit, only works when the limit
    // is a non-negative value.
    if (count.has_value() && keys.size() >= *count) {
      return keys;
    }
    ++start;
    // Add the key if the current rank is larger of equal to the specified
    // offset
    if (i++ >= offset) {
      keys.push_back(node->key);
    }
    node = node->Prev();
  }
  return keys;
}

/*
 * Find keys within the given range of keys.
 * The function assumes spec is valid. Should call ValidateRangeKeySpec
 * before calling this function.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key> Skiplist<Key, Comparator, Destructor>::RangeByKeyWithValidSpec(
    const SkiplistRangeByKeySpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) return {};
  const SkiplistNode* node = FindKeyGreaterOrEqual(spec->min);
  if (!node) {
    return {};
  }
  // If min exclusive and the node is equal to min, then move to the next node.
  if (spec->minex && Eq(node->key, spec->min)) {
    node = node->Next(0);
  }
  size_t i = 0, offset = spec->limit ? spec->limit->offset : 0;
  std::vector<Key> keys;
  while (node &&
         (spec->maxex ? Lt(node->key, spec->max) : Lte(node->key, spec->max))) {
    // Return the keys if the number reach the limit, only works when the limit
    // is a non-negative value.
    if (count.has_value() && keys.size() >= *count) {
      return keys;
    }
    // Add the key if the current rank is larger of equal to the specified
    // offset.
    if (i++ >= offset) {
      keys.push_back(node->key);
    }
    node = node->Next(0);
  }
  return keys;
}

/*
 * Find keys within the given range of keys.
 * The function assumes spec is valid. Should call ValidateRangeKeySpec
 * before calling this function.
 */
template <typename Key, typename Comparator, typename Destructor>
std::vector<Key>
Skiplist<Key, Comparator, Destructor>::RevRangeByKeyWithValidSpec(
    const SkiplistRangeByKeySpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) return {};
  const SkiplistNode* node = FindKeyLessOrEqual(spec->max);
  if (!node) {
    return {};
  }
  // If max exclusive and the node is equal to max, then move to the previous
  // node
  if (spec->maxex && Eq(node->key, spec->max)) {
    node = node->Prev();
  }
  size_t i = 0, offset = spec->limit ? spec->limit->offset : 0;
  std::vector<Key> keys;
  while (node != head_ &&
         (spec->minex ? Gt(node->key, spec->min) : Gte(node->key, spec->min))) {
    // Return the keys if the number reach the limit, only works when the limit
    // is a non-negative value.
    if (count.has_value() && keys.size() >= *count) {
      return keys;
    }
    // Add the key if the current rank is larger of equal to the specified
    // offset.
    if (i++ >= offset) {
      keys.push_back(node->key);
    }
    node = node->Prev();
  }
  return keys;
}

/*
 * Find number of keys within the given range of keys.
 * The function assumes spec is valid. Should call ValidateRangeKeySpec
 * before calling this function.
 * The result is not related to SkiplistRangeByKeySpec.SkiplistLimitSpec.
 */
template <typename Key, typename Comparator, typename Destructor>
size_t Skiplist<Key, Comparator, Destructor>::CountWithValidSpec(
    const SkiplistRangeByKeySpec* spec) const {
  const SkiplistNode* node = FindKeyGreaterOrEqual(spec->min);
  if (!node) {
    return 0;
  }
  // If min exclusive and the node is equal to min, then move to the next node.
  if (spec->minex && Eq(node->key, spec->min)) {
    node = node->Next(0);
  }
  size_t num = 0;
  std::vector<Key> keys;
  while (node &&
         (spec->maxex ? Lt(node->key, spec->max) : Lte(node->key, spec->max))) {
    node = node->Next(0);
    ++num;
  }
  return num;
}

/*
 * Seek to the last Key of the skiplist
 */
template <typename Key, typename Comparator, typename Destructor>
const typename Skiplist<Key, Comparator, Destructor>::SkiplistNode*
Skiplist<Key, Comparator, Destructor>::FindLast() const {
  const SkiplistNode* node = head_;
  int l = level_;
  while (--l >= 0) {
    while (node->Next(l)) {
      node = node->Next(l);
    }
  }
  return node;
}

/*
 * Reset the skiplist to the initial state.
 */
template <typename Key, typename Comparator, typename Destructor>
void Skiplist<Key, Comparator, Destructor>::Reset() {
  SkiplistNode* node = head_->Next(0);
  while (node) {
    SkiplistNode* next = node->Next(0);
    delete node;
    node = next;
  }
  head_->Reset();
  size_ = 0;
}

template <typename Key, typename Comparator, typename Destructor>
Skiplist<Key, Comparator, Destructor>::~Skiplist() {
  Reset();
  delete head_;
}
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_SKIPLIST_H_
