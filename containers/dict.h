#ifndef REDIS_SIMPLE_CONTAINERS_DICT_H_
#define REDIS_SIMPLE_CONTAINERS_DICT_H_

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <concepts>
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

namespace redis_simple::containers {
// Default hash and key-equality policies. The std::string specializations
// accept std::string_view so lookups by borrowed key do not allocate.
template <typename K>
struct DictHash {
  size_t operator()(const K& key) const { return std::hash<K>{}(key); }
};

// A string key with its DictHash<std::string> hash. Every Dict using that
// policy shares one process-wide seed, so a caller can hash a key once and
// look it up in several such Dicts. Other hash policies reject it at compile
// time instead of mixing incompatible hashes.
struct HashedKey {
  explicit HashedKey(std::string_view key_view)
      : key(key_view), hash(static_cast<size_t>(utils::SeededHash(key_view))) {}

  std::string_view key;
  size_t hash;

 private:
  // Dict iterators rebuild a key from the hash stored with its entry.
  template <typename, typename, typename, typename>
  friend class Dict;
  HashedKey(std::string_view key_view, size_t stored_hash)
      : key(key_view), hash(stored_hash) {}
};

// String keys usually come from clients, so they use the process-seeded
// SipHash to resist hash-flooding attacks.
template <>
struct DictHash<std::string> {
  size_t operator()(std::string_view key) const {
    return static_cast<size_t>(utils::SeededHash(key));
  }
  size_t operator()(const HashedKey& key) const { return key.hash; }
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
  bool operator()(const HashedKey& left, std::string_view right) const {
    return left.key == right;
  }
};

// A Dict hash policy maps a key to a bucket hash.
template <typename Hash, typename K>
concept DictHashFor =
    std::copy_constructible<Hash> && requires(const Hash& hash, const K& key) {
      { hash(key) } -> std::convertible_to<size_t>;
    };

// A Dict equality policy decides whether two keys are the same entry.
template <typename KeyEqual, typename K>
concept DictKeyEqualFor = std::copy_constructible<KeyEqual> &&
                          std::predicate<const KeyEqual&, const K&, const K&>;

// Incrementally rehashed chained hash table modeled on Redis' dict. The
// policies are template parameters so hashing and comparison inline.
template <typename K, typename V, typename Hash = DictHash<K>,
          typename KeyEqual = DictKeyEqual<K>>
class Dict {
  // Checked here rather than on the template parameters so the out-of-line
  // member definitions keep their short template heads.
  static_assert(DictHashFor<Hash, K>,
                "Hash must map a const K& to a size_t hash");
  static_assert(DictKeyEqualFor<KeyEqual, K>,
                "KeyEqual must compare two const K& keys");

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
  V* FindValue(const HashedKey& key) { return FindValueImpl(key); }
  // Returns the value for `key`, inserting a value-initialized one if the
  // key is new. Hashes the key once and copies it only when inserting.
  template <typename Lookup>
  std::pair<V*, bool> FindOrInsert(const Lookup& key);
  void Set(const K& key, const V& value);
  void Set(const K& key, V&& value);
  void Set(K&& key, V&& value);
  bool Insert(const K& key, const V& value);
  bool Insert(K&& key, V&& value);
  bool Delete(const K& key) { return DeleteImpl(key); }
  bool Delete(std::string_view key) { return DeleteImpl(key); }
  bool Delete(const char* key) { return Delete(std::string_view(key)); }
  bool Delete(const HashedKey& key) { return DeleteImpl(key); }
  std::optional<V> Extract(const K& key) { return ExtractImpl(key); }
  std::optional<V> Extract(std::string_view key) { return ExtractImpl(key); }
  std::optional<V> Extract(const char* key) {
    return Extract(std::string_view(key));
  }
  std::optional<V> Extract(const HashedKey& key) { return ExtractImpl(key); }
  // Visits one bucket position and returns the next cursor, or nullopt once
  // the scan is complete. Cursors use Redis' reverse-binary order, so every
  // element present for the whole scan is visited even if the table grows or
  // shrinks between calls.
  template <typename Visitor>
  std::optional<size_t> Scan(size_t cursor, Visitor&& visitor);
  // One step of a SCAN-family command, as Redis' scanGenericCommand runs it:
  // scans buckets from `cursor` until `count` elements have been visited,
  // the scan completes, or 10 * `count` + 1 buckets have been scanned, which
  // bounds the work in a sparse table. Returns the next cursor, 0 once the
  // scan is complete. The visitor takes (key, value); for string keys it
  // may take (HashedKey, value) instead to reuse each entry's stored hash.
  template <typename Visitor>
  size_t ScanStep(size_t cursor, size_t count, Visitor&& visitor);
  size_t Size() const { return table_used_[0] + table_used_[1]; }
  void Clear();
  ~Dict();

 private:
  struct DictEntry {
    K key;
    V value;
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
  bool IsRehashing() const { return rehash_index_.has_value(); }
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
  std::optional<size_t> rehash_index_;
  size_t pause_rehash_{};
};

template <typename K, typename V, typename Hash, typename KeyEqual>
class Dict<K, V, Hash, KeyEqual>::Iterator {
 public:
  explicit Iterator(const Dict* dict);
  Iterator& operator=(const Iterator& it) = default;
  bool operator==(const Iterator& it) const = default;
  bool Valid() const;
  void SeekToFirst();
  void SeekToLast();
  void Next();
  Iterator& operator++();
  const K& Key() const { return entry_->key; }
  const V& Value() const { return entry_->value; }
  // The current key with the hash stored in its entry, so it can be looked
  // up in other Dicts that share the seeded string hash without rehashing.
  HashedKey Hashed() const
    requires std::same_as<Hash, DictHash<std::string>>
  {
    return HashedKey(entry_->key, entry_->hash);
  }

 private:
  void SeekToNextEntry();
  const Dict* dict_;
  int table_;
  std::optional<size_t> index_;
  const DictEntry* entry_;
};

template <typename K, typename V, typename Hash, typename KeyEqual>
Dict<K, V, Hash, KeyEqual>::Iterator::Iterator(const Dict* dict)
    : dict_(dict), table_(0), index_(std::nullopt), entry_(nullptr) {}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Iterator::Valid() const {
  return entry_ != nullptr;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Iterator::SeekToFirst() {
  table_ = 0;
  index_ = std::nullopt;
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
      index_ = index;
      entry_ = dict_->tables_[table_][index];
      while (entry_->next != nullptr) {
        entry_ = entry_->next;
      }
      return;
    }
  }
  table_ = 0;
  index_ = std::nullopt;
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
  index_ = index_.has_value() ? std::optional<size_t>(*index_ + 1) : size_t{0};
  // Find next non empty table entry list.
  while (*index_ < dict_->tables_[table_].size() &&
         dict_->tables_[table_][*index_] == nullptr) {
    ++*index_;
  }
  if (*index_ < dict_->tables_[table_].size()) {
    entry_ = dict_->tables_[table_][*index_];
    return;
  }
  if (table_ == 1 || !dict_->IsRehashing()) {
    entry_ = nullptr;
    return;
  }
  // Find the next element in the rehashed table.
  index_ = std::nullopt;
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
  // Like Redis' dictFind, an empty dict answers without hashing the key,
  // which keeps lookups in an empty TTL table nearly free.
  if (Size() == 0) {
    return nullptr;
  }
  RehashStepIfNeeded();
  DictEntry* const entry = FindEntry(key, KeyHash(key));
  return entry == nullptr ? nullptr : &entry->value;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
std::pair<V*, bool> Dict<K, V, Hash, KeyEqual>::FindOrInsert(
    const Lookup& key) {
  ExpandIfNeeded();
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  if (DictEntry* const existing = FindEntry(key, hash); existing != nullptr) {
    return {&existing->value, false};
  }
  return {&InsertNew(key, hash)->value, true};
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Set(const K& key, const V& value) {
  *FindOrInsert(key).first = value;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Set(const K& key, V&& value) {
  *FindOrInsert(key).first = std::move(value);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Set(K&& key, V&& value) {
  ExpandIfNeeded();
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  if (DictEntry* const existing = FindEntry(key, hash); existing != nullptr) {
    existing->value = std::move(value);
    return;
  }
  auto entry = std::make_unique<DictEntry>();
  entry->key = std::move(key);
  entry->value = std::move(value);
  entry->hash = hash;
  InsertEntry(entry.release(), IsRehashing() ? 1 : 0);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Insert(const K& key, const V& value) {
  auto [slot, inserted] = FindOrInsert(key);
  if (inserted) {
    *slot = value;
  }
  return inserted;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Insert(K&& key, V&& value) {
  ExpandIfNeeded();
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  if (FindEntry(key, hash) != nullptr) {
    return false;
  }
  auto entry = std::make_unique<DictEntry>();
  entry->key = std::move(key);
  entry->value = std::move(value);
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
  std::optional<V> value(std::move(entry->value));
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
template <typename Visitor>
size_t Dict<K, V, Hash, KeyEqual>::ScanStep(size_t cursor, size_t count,
                                            Visitor&& visitor) {
  constexpr size_t kMaxSize = std::numeric_limits<size_t>::max();
  size_t iterations_left = count > kMaxSize / 10 ? kMaxSize : count * 10;
  size_t visited = 0;
  // Accepts the key in whichever form the visitor takes it; see VisitBucket.
  const auto visit = [&visited, &visitor](const auto& key, const V& value)
    requires std::invocable<Visitor&, decltype(key), const V&>
  {
    ++visited;
    visitor(key, value);
  };
  // One call site, so the scan inlines with the caller's visitor.
  std::optional<size_t> next_cursor = cursor;
  while (true) {
    next_cursor = Scan(*next_cursor, visit);
    if (!next_cursor.has_value() || iterations_left == 0 || visited >= count) {
      return next_cursor.value_or(0);
    }
    --iterations_left;
  }
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::Clear() {
  ClearTable(0);
  ClearTable(1);
  rehash_index_ = std::nullopt;
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
  if (Size() == 0) {
    return nullptr;
  }
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
  if constexpr (std::same_as<Lookup, HashedKey>) {
    entry->key = K(key.key);
  } else {
    entry->key = K(key);
  }
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
  // Largest power-of-two bucket count whose table size fits in size_t.
  constexpr size_t kMaxTableSize =
      std::bit_floor(std::numeric_limits<size_t>::max() / sizeof(DictEntry*));
  if (size > kMaxTableSize) {
    return false;
  }
  const size_t new_size = std::max(kTableInitSize, std::bit_ceil(size));
  if (new_size == TableSize(0)) {
    return false;
  }
  if (tables_[0].empty()) {
    tables_[0].assign(new_size, nullptr);
    return true;
  }
  tables_[1].assign(new_size, nullptr);
  table_used_[1] = 0;
  rehash_index_ = size_t{0};
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
  while (n > 0 && *rehash_index_ < old_size && table_used_[0] > 0 &&
         empty_visits > 0) {
    DictEntry* entry = tables_[0][*rehash_index_];
    if (entry == nullptr) {
      --empty_visits;
      ++*rehash_index_;
      continue;
    }
    while (entry != nullptr) {
      DictEntry* const next = entry->next;
      --table_used_[0];
      InsertEntry(entry, 1);
      entry = next;
    }
    tables_[0][*rehash_index_] = nullptr;
    --n;
    ++*rehash_index_;
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
  rehash_index_ = std::nullopt;
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
    if constexpr (std::invocable<Visitor&, const K&, const V&>) {
      visitor(entry->key, entry->value);
    } else {
      // A visitor that takes a HashedKey gets the hash stored with the
      // entry, so it can look the key up in another Dict without rehashing.
      visitor(HashedKey(entry->key, entry->hash), entry->value);
    }
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
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_DICT_H_
