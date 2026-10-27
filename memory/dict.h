#ifndef REDIS_SIMPLE_MEMORY_DICT_H_
#define REDIS_SIMPLE_MEMORY_DICT_H_

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "utils/siphash.h"

namespace redis_simple::in_memory {
// Default hash and key-equality policies. The std::string specializations
// accept std::string_view so lookups by borrowed key do not allocate.
template <typename K>
struct DictHash {
  size_t operator()(const K& key) const { return std::hash<K>{}(key); }
};

// String keys usually come from clients, so they use the process-seeded
// SipHash to resist hash-flooding attacks.
template <>
struct DictHash<std::string> {
  size_t operator()(std::string_view key) const {
    return static_cast<size_t>(utils::SeededHash(key));
  }
};

template <typename K>
struct DictKeyEqual {
  bool operator()(const K& left, const K& right) const { return left == right; }
};

template <>
struct DictKeyEqual<std::string> {
  bool operator()(std::string_view left, std::string_view right) const {
    return left == right;
  }
};

// Incrementally rehashed chained hash table modeled on Redis' dict. The
// policies are template parameters so hashing and comparison inline.
template <typename K, typename V, typename Hash = DictHash<K>,
          typename KeyEqual = DictKeyEqual<K>>
class Dict {
 public:
  class Iterator;
  static std::unique_ptr<Dict> Create();
  static std::unique_ptr<Dict> Create(size_t capacity);
  Dict(const Dict&) = delete;
  Dict& operator=(const Dict&) = delete;
  std::optional<V> Get(const K& key);
  V* FindValue(const K& key) { return FindValueImpl(key); }
  V* FindValue(std::string_view key) { return FindValueImpl(key); }
  V* FindValue(const char* key) { return FindValue(std::string_view(key)); }
  // Returns the value for `key`, inserting a value-initialized one if the
  // key is new. Hashes the key once and copies it only when inserting.
  template <typename Lookup>
  std::pair<V*, bool> FindOrInsert(const Lookup& key);
  void Set(const K& key, const V& val);
  void Set(const K& key, V&& val);
  void Set(K&& key, V&& val);
  bool Insert(const K& key, const V& val);
  bool Insert(K&& key, V&& val);
  bool Delete(const K& key) { return DeleteImpl(key); }
  bool Delete(std::string_view key) { return DeleteImpl(key); }
  bool Delete(const char* key) { return Delete(std::string_view(key)); }
  std::optional<V> Extract(const K& key) { return ExtractImpl(key); }
  std::optional<V> Extract(std::string_view key) { return ExtractImpl(key); }
  std::optional<V> Extract(const char* key) {
    return Extract(std::string_view(key));
  }
  // Visits one bucket position and returns the next cursor, or nullopt once
  // the scan is complete. Cursors use Redis' reverse-binary order, so every
  // element present for the whole scan is visited even if the table grows or
  // shrinks between calls.
  template <typename Visitor>
  std::optional<size_t> Scan(size_t cursor, Visitor&& visitor);
  size_t Size() const { return table_used_[0] + table_used_[1]; }
  void Clear();
  ~Dict();

 private:
  struct DictEntry {
    K key;
    V val;
    size_t hash{};
    DictEntry* next{nullptr};
  };

  static constexpr int kTableInitExp = 1;
  static constexpr size_t kTableInitSize = size_t{1} << kTableInitExp;
  // Shrink once fewer than 1/kShrinkRatio of the buckets are in use.
  static constexpr size_t kShrinkRatio = 8;

  Dict();
  template <typename Lookup>
  V* FindValueImpl(const Lookup& key);
  template <typename Lookup>
  bool DeleteImpl(const Lookup& key);
  template <typename Lookup>
  std::optional<V> ExtractImpl(const Lookup& key);
  template <typename Lookup>
  DictEntry* FindEntry(const Lookup& key, size_t hash) const;
  template <typename Lookup>
  DictEntry* Unlink(const Lookup& key);
  template <typename Lookup>
  DictEntry* InsertNew(const Lookup& key, size_t hash);
  template <typename Lookup>
  bool KeyEquals(const Lookup& key, const DictEntry* entry) const;
  template <typename Lookup>
  size_t KeyHash(const Lookup& key) const;
  void InsertEntry(DictEntry* entry, int table);
  void UnlinkEntry(DictEntry* entry, DictEntry* prev, int table);
  size_t TableSize(int table) const { return tables_[table].size(); }
  size_t TableMask(int table) const {
    return tables_[table].empty() ? 0 : tables_[table].size() - 1;
  }
  bool IsRehashing() const { return rehash_idx_.has_value(); }
  void PauseRehashing() { ++pause_rehash_; }
  void ResumeRehashing() {
    if (pause_rehash_ > 0) {
      --pause_rehash_;
    }
  }
  void ExpandIfNeeded();
  void ShrinkIfNeeded();
  bool Resize(size_t size);
  void RehashStepIfNeeded();
  void Rehash(int n);
  void MigrateRehashedTable();
  void ClearTable(int table);
  template <typename Visitor>
  static void VisitBucket(const DictEntry* entry, Visitor& visitor);
  static size_t NextCursor(size_t cursor, size_t mask);
  static size_t ReverseBits(size_t value);

  Hash hash_;
  KeyEqual key_equal_;
  // Table 1 is populated incrementally while table 0 is being rehashed.
  std::array<std::vector<DictEntry*>, 2> tables_;
  std::array<size_t, 2> table_used_{};
  std::optional<size_t> rehash_idx_;
  size_t pause_rehash_{};
};

template <typename K, typename V, typename Hash, typename KeyEqual>
class Dict<K, V, Hash, KeyEqual>::Iterator {
 public:
  explicit Iterator(const Dict* dict);
  Iterator& operator=(const Iterator& it) = default;
  bool operator==(const Iterator& it) const;
  bool operator!=(const Iterator& it) const;
  bool Valid() const;
  void SeekToFirst();
  void SeekToLast();
  void Next();
  Iterator& operator++();
  const K& Key() const { return entry_->key; }
  const V& Value() const { return entry_->val; }

 private:
  void SeekToNextEntry();
  const Dict* dict_;
  int table_;
  std::optional<size_t> idx_;
  const DictEntry* entry_;
};

template <typename K, typename V, typename Hash, typename KeyEqual>
Dict<K, V, Hash, KeyEqual>::Iterator::Iterator(const Dict* dict)
    : dict_(dict), table_(0), idx_(std::nullopt), entry_(nullptr) {}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Iterator::operator==(
    const Iterator& it) const {
  return dict_ == it.dict_ && table_ == it.table_ && idx_ == it.idx_ &&
         entry_ == it.entry_;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Iterator::operator!=(
    const Iterator& it) const {
  return !((*this) == it);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Iterator::Valid() const {
  return entry_ != nullptr;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Iterator::SeekToFirst() {
  table_ = 0;
  idx_ = std::nullopt;
  entry_ = nullptr;
  SeekToNextEntry();
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Iterator::SeekToLast() {
  entry_ = nullptr;
  const int last_table = dict_->IsRehashing() ? 1 : 0;
  for (table_ = last_table; table_ >= 0; --table_) {
    for (size_t bucket = dict_->tables_[table_].size(); bucket > 0; --bucket) {
      const size_t index = bucket - 1;
      if (dict_->tables_[table_][index] == nullptr) {
        continue;
      }
      idx_ = index;
      entry_ = dict_->tables_[table_][index];
      while (entry_->next != nullptr) {
        entry_ = entry_->next;
      }
      return;
    }
  }
  table_ = 0;
  idx_ = std::nullopt;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
typename Dict<K, V, Hash, KeyEqual>::Iterator&
Dict<K, V, Hash, KeyEqual>::Iterator::operator++() {
  SeekToNextEntry();
  return *this;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Iterator::Next() {
  SeekToNextEntry();
}

// Finds the next non-null entry in the dict.
template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Iterator::SeekToNextEntry() {
  if (entry_ && entry_->next) {
    entry_ = entry_->next;
    return;
  }
  idx_ = idx_.has_value() ? std::optional<size_t>(*idx_ + 1) : size_t{0};
  // Find next non empty table entry list.
  while (*idx_ < dict_->tables_[table_].size() &&
         dict_->tables_[table_][*idx_] == nullptr) {
    ++*idx_;
  }
  if (*idx_ < dict_->tables_[table_].size()) {
    entry_ = dict_->tables_[table_][*idx_];
    return;
  }
  if (table_ == 1 || !dict_->IsRehashing()) {
    entry_ = nullptr;
    return;
  }
  // Find the next element in the rehashed table.
  idx_ = std::nullopt;
  table_ = 1;
  SeekToNextEntry();
}

template <typename K, typename V, typename Hash, typename KeyEqual>
std::unique_ptr<Dict<K, V, Hash, KeyEqual>>
Dict<K, V, Hash, KeyEqual>::Create() {
  return Create(kTableInitSize);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
std::unique_ptr<Dict<K, V, Hash, KeyEqual>> Dict<K, V, Hash, KeyEqual>::Create(
    size_t capacity) {
  std::unique_ptr<Dict> dict(new Dict());
  if (!dict->Resize(std::max(capacity, kTableInitSize))) {
    return nullptr;
  }
  return dict;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
Dict<K, V, Hash, KeyEqual>::Dict() = default;

template <typename K, typename V, typename Hash, typename KeyEqual>
Dict<K, V, Hash, KeyEqual>::~Dict() {
  Clear();
}

template <typename K, typename V, typename Hash, typename KeyEqual>
std::optional<V> Dict<K, V, Hash, KeyEqual>::Get(const K& key) {
  const V* const value = FindValue(key);
  if (value == nullptr) {
    return std::nullopt;
  }
  return *value;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
V* Dict<K, V, Hash, KeyEqual>::FindValueImpl(const Lookup& key) {
  RehashStepIfNeeded();
  DictEntry* const entry = FindEntry(key, KeyHash(key));
  return entry == nullptr ? nullptr : &entry->val;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
std::pair<V*, bool> Dict<K, V, Hash, KeyEqual>::FindOrInsert(
    const Lookup& key) {
  ExpandIfNeeded();
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  if (DictEntry* const existing = FindEntry(key, hash); existing != nullptr) {
    return {&existing->val, false};
  }
  return {&InsertNew(key, hash)->val, true};
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Set(const K& key, const V& val) {
  *FindOrInsert(key).first = val;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Set(const K& key, V&& val) {
  *FindOrInsert(key).first = std::move(val);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Set(K&& key, V&& val) {
  ExpandIfNeeded();
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  if (DictEntry* const existing = FindEntry(key, hash); existing != nullptr) {
    existing->val = std::move(val);
    return;
  }
  auto entry = std::make_unique<DictEntry>();
  entry->key = std::move(key);
  entry->val = std::move(val);
  entry->hash = hash;
  InsertEntry(entry.release(), IsRehashing() ? 1 : 0);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Insert(const K& key, const V& val) {
  auto [value, inserted] = FindOrInsert(key);
  if (inserted) {
    *value = val;
  }
  return inserted;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Insert(K&& key, V&& val) {
  ExpandIfNeeded();
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  if (FindEntry(key, hash) != nullptr) {
    return false;
  }
  auto entry = std::make_unique<DictEntry>();
  entry->key = std::move(key);
  entry->val = std::move(val);
  entry->hash = hash;
  InsertEntry(entry.release(), IsRehashing() ? 1 : 0);
  return true;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
bool Dict<K, V, Hash, KeyEqual>::DeleteImpl(const Lookup& key) {
  std::unique_ptr<DictEntry> entry(Unlink(key));
  if (entry == nullptr) {
    return false;
  }
  entry.reset();
  ShrinkIfNeeded();
  return true;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
std::optional<V> Dict<K, V, Hash, KeyEqual>::ExtractImpl(const Lookup& key) {
  std::unique_ptr<DictEntry> entry(Unlink(key));
  if (entry == nullptr) {
    return std::nullopt;
  }
  std::optional<V> value(std::move(entry->val));
  entry.reset();
  ShrinkIfNeeded();
  return value;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Visitor>
std::optional<size_t> Dict<K, V, Hash, KeyEqual>::Scan(size_t cursor,
                                                       Visitor&& visitor) {
  if (Size() == 0) {
    return std::nullopt;
  }
  // Entries must not move between tables while one cursor step runs.
  PauseRehashing();
  if (!IsRehashing()) {
    const size_t mask = TableMask(0);
    VisitBucket(tables_[0][cursor & mask], visitor);
    cursor = NextCursor(cursor, mask);
  } else {
    // Visit the bucket in the smaller table, then every bucket of the larger
    // table that it expands to.
    const int small = TableSize(0) <= TableSize(1) ? 0 : 1;
    const int large = 1 - small;
    const size_t small_mask = TableMask(small);
    const size_t large_mask = TableMask(large);
    VisitBucket(tables_[small][cursor & small_mask], visitor);
    do {
      VisitBucket(tables_[large][cursor & large_mask], visitor);
      cursor = NextCursor(cursor, large_mask);
    } while ((cursor & (small_mask ^ large_mask)) != 0);
  }
  ResumeRehashing();
  if (cursor == 0) {
    return std::nullopt;
  }
  return cursor;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Clear() {
  ClearTable(0);
  ClearTable(1);
  rehash_idx_ = std::nullopt;
  pause_rehash_ = 0;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
size_t Dict<K, V, Hash, KeyEqual>::KeyHash(const Lookup& key) const {
  return hash_(key);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
bool Dict<K, V, Hash, KeyEqual>::KeyEquals(const Lookup& key,
                                           const DictEntry* entry) const {
  return key_equal_(key, entry->key);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
typename Dict<K, V, Hash, KeyEqual>::DictEntry*
Dict<K, V, Hash, KeyEqual>::FindEntry(const Lookup& key, size_t hash) const {
  for (int table = 0; table <= 1; ++table) {
    if (tables_[table].empty()) {
      break;
    }
    for (DictEntry* entry = tables_[table][hash & TableMask(table)];
         entry != nullptr; entry = entry->next) {
      if (entry->hash == hash && KeyEquals(key, entry)) {
        return entry;
      }
    }
    if (!IsRehashing()) {
      break;
    }
  }
  return nullptr;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
typename Dict<K, V, Hash, KeyEqual>::DictEntry*
Dict<K, V, Hash, KeyEqual>::Unlink(const Lookup& key) {
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  for (int table = 0; table <= 1; ++table) {
    if (tables_[table].empty()) {
      break;
    }
    DictEntry* prev = nullptr;
    for (DictEntry* entry = tables_[table][hash & TableMask(table)];
         entry != nullptr; prev = entry, entry = entry->next) {
      if (entry->hash == hash && KeyEquals(key, entry)) {
        UnlinkEntry(entry, prev, table);
        return entry;
      }
    }
    if (!IsRehashing()) {
      break;
    }
  }
  return nullptr;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
typename Dict<K, V, Hash, KeyEqual>::DictEntry*
Dict<K, V, Hash, KeyEqual>::InsertNew(const Lookup& key, size_t hash) {
  auto entry = std::make_unique<DictEntry>();
  entry->key = K(key);
  entry->hash = hash;
  DictEntry* const raw = entry.release();
  InsertEntry(raw, IsRehashing() ? 1 : 0);
  return raw;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::InsertEntry(DictEntry* entry, int table) {
  DictEntry*& bucket = tables_[table][entry->hash & TableMask(table)];
  entry->next = bucket;
  bucket = entry;
  ++table_used_[table];
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::UnlinkEntry(DictEntry* entry, DictEntry* prev,
                                             int table) {
  if (prev != nullptr) {
    prev->next = entry->next;
  } else {
    tables_[table][entry->hash & TableMask(table)] = entry->next;
  }
  entry->next = nullptr;
  --table_used_[table];
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::ExpandIfNeeded() {
  // Keep chains short: grow once there is one entry per bucket.
  if (!IsRehashing() && table_used_[0] >= TableSize(0)) {
    Resize(table_used_[0] + 1);
  }
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::ShrinkIfNeeded() {
  if (!IsRehashing() && pause_rehash_ == 0 && TableSize(0) > kTableInitSize &&
      table_used_[0] * kShrinkRatio <= TableSize(0)) {
    Resize(table_used_[0]);
  }
}

// Starts moving entries into a table sized to the next power of two at or
// above `size`. The first call allocates table 0 directly.
template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Resize(size_t size) {
  if (IsRehashing()) {
    return false;
  }
  size_t new_size = kTableInitSize;
  while (new_size < size) {
    if (new_size >
        std::numeric_limits<size_t>::max() / 2 / sizeof(DictEntry*)) {
      return false;
    }
    new_size *= 2;
  }
  if (new_size == TableSize(0)) {
    return false;
  }
  if (tables_[0].empty()) {
    tables_[0].assign(new_size, nullptr);
    return true;
  }
  tables_[1].assign(new_size, nullptr);
  table_used_[1] = 0;
  rehash_idx_ = size_t{0};
  return true;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::RehashStepIfNeeded() {
  if (pause_rehash_ == 0) {
    Rehash(1);
  }
}

// Performs at most n non-empty bucket migrations. Empty buckets are skipped
// with a bounded visit count so sparse tables cannot stall one operation.
template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Rehash(int n) {
  if (!IsRehashing()) {
    return;
  }
  int empty_visits = n * 10;
  const size_t old_size = TableSize(0);
  while (n > 0 && *rehash_idx_ < old_size && table_used_[0] > 0 &&
         empty_visits > 0) {
    DictEntry* entry = tables_[0][*rehash_idx_];
    if (entry == nullptr) {
      --empty_visits;
      ++*rehash_idx_;
      continue;
    }
    while (entry != nullptr) {
      DictEntry* const next = entry->next;
      --table_used_[0];
      InsertEntry(entry, 1);
      entry = next;
    }
    tables_[0][*rehash_idx_] = nullptr;
    --n;
    ++*rehash_idx_;
  }
  if (table_used_[0] == 0) {
    MigrateRehashedTable();
  }
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::MigrateRehashedTable() {
  tables_[0] = std::move(tables_[1]);
  table_used_[0] = table_used_[1];
  tables_[1].clear();
  table_used_[1] = 0;
  rehash_idx_ = std::nullopt;
  // Deletes cannot start a shrink while a rehash is in flight, so a table that
  // emptied out during this rehash would otherwise stay oversized.
  ShrinkIfNeeded();
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::ClearTable(int table) {
  for (DictEntry* const bucket : tables_[table]) {
    DictEntry* entry = bucket;
    while (entry != nullptr) {
      DictEntry* const next = entry->next;
      delete entry;
      entry = next;
    }
  }
  table_used_[table] = 0;
  // Release the buckets too; the next insert allocates a minimal table.
  std::vector<DictEntry*>().swap(tables_[table]);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Visitor>
void Dict<K, V, Hash, KeyEqual>::VisitBucket(const DictEntry* entry,
                                             Visitor& visitor) {
  while (entry != nullptr) {
    const DictEntry* const next = entry->next;
    visitor(entry->key, entry->val);
    entry = next;
  }
}

// Increments the high bits of `cursor` first, as in Redis' dictScan.
template <typename K, typename V, typename Hash, typename KeyEqual>
size_t Dict<K, V, Hash, KeyEqual>::NextCursor(size_t cursor, size_t mask) {
  cursor |= ~mask;
  cursor = ReverseBits(cursor);
  ++cursor;
  return ReverseBits(cursor);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
size_t Dict<K, V, Hash, KeyEqual>::ReverseBits(size_t value) {
  size_t shift = sizeof(value) * CHAR_BIT;
  size_t mask = ~size_t{0};
  while ((shift >>= 1) > 0) {
    mask ^= (mask << shift);
    value = ((value >> shift) & mask) | ((value << shift) & ~mask);
  }
  return value;
}
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_DICT_H_
