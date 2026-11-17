#ifndef REDIS_SIMPLE_CONTAINERS_DICT_H_
#define REDIS_SIMPLE_CONTAINERS_DICT_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "utils/siphash.h"

namespace redis_simple::containers {
// Whether Dicts may resize and rehash, for the whole process, as Redis'
// dict_can_resize is. Rehashing writes to every entry it moves, and while a
// forked child shares the parent's memory each written page is copied. So
// the parent avoids it while a child lives, resizing only a table that is
// badly out of size, and the child, which only reads, never does it.
enum class DictResizePolicy { kEnabled, kAvoid, kForbidden };

namespace dict_internal {
// Only the main thread sets this, and Dicts are used and freed there too
// (UNLINKed values are freed in steps on the main thread), so the atomic is
// a safeguard rather than a requirement.
inline std::atomic<DictResizePolicy> resize_policy{DictResizePolicy::kEnabled};
}  // namespace dict_internal

inline void SetDictResizePolicy(DictResizePolicy policy) {
  dict_internal::resize_policy.store(policy, std::memory_order_relaxed);
}

inline DictResizePolicy CurrentDictResizePolicy() {
  return dict_internal::resize_policy.load(std::memory_order_relaxed);
}

// Default hash and key-equality policies. The std::string specializations
// accept std::string_view so lookups by borrowed key do not allocate.
template <typename K>
struct DictHash {
  size_t operator()(const K& key) const { return std::hash<K>{}(key); }
};

namespace dict_internal {
// Entries keep the low 32 bits of their key's hash: enough to pick a bucket
// in any table this server builds and to skip most unequal keys without
// comparing them, and with a 32-bit length it saves 8 bytes per string-keyed
// entry. Dict truncates every hash it computes, so a hash read back from an
// entry finds the same key in another Dict.
using EntryHash = uint32_t;
}  // namespace dict_internal

// A string key with its DictHash<std::string> hash. Every Dict using that
// policy shares one process-wide seed, so a caller can hash a key once and
// look it up in several such Dicts. Other hash policies reject it at compile
// time instead of mixing incompatible hashes.
struct HashedKey {
  explicit HashedKey(std::string_view key_view)
      : key(key_view),
        hash(static_cast<dict_internal::EntryHash>(
            utils::SeededHash(key_view))) {}

  std::string_view key;
  dict_internal::EntryHash hash;

 private:
  // Dict iterators rebuild a key from the hash stored with its entry.
  template <typename, typename, typename, typename>
  friend class Dict;
  HashedKey(std::string_view key_view, size_t stored_hash)
      : key(key_view),
        hash(static_cast<dict_internal::EntryHash>(stored_hash)) {}
};

// String keys usually come from clients, so they use the process-seeded
// SipHash to resist hash-flooding attacks.
template <>
struct DictHash<std::string> {
  size_t operator()(std::string_view key) const {
    return static_cast<dict_internal::EntryHash>(utils::SeededHash(key));
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

// The value type of a Dict used as a set: entries hold no value at all, not
// even the 8 bytes a std::nullptr_t would take.
struct NoValue {};

namespace dict_internal {
// One key and value in a bucket's chain. `KeyView` is how the key is read
// back: a reference to the stored key.
template <typename K, typename V>
struct DictEntry {
  using KeyView = const K&;

  template <typename Source>
  static DictEntry* Create(Source&& key, size_t hash) {
    return new DictEntry{.key = K(std::forward<Source>(key)),
                         .value = V(),
                         .hash = hash,
                         .next = nullptr};
  }
  static void Destroy(DictEntry* entry) { delete entry; }
  KeyView Key() const { return key; }

  K key;
  // Takes no space when V is empty (NoValue).
  [[no_unique_address]] V value;
  size_t hash;
  DictEntry* next;
};

// A string key's bytes follow its entry in the same allocation, as Redis
// embeds keys in its entries, so a key costs no allocation of its own and no
// std::string header. The key is read back as a std::string_view, which
// stays valid until the entry is deleted: rehashing relinks entries without
// moving them.
template <typename V>
struct DictEntry<std::string, V> {
  using KeyView = std::string_view;

  static DictEntry* Create(std::string_view key, size_t hash) {
    static_assert(
        !std::is_empty_v<V> || sizeof(DictEntry) == 2 * sizeof(size_t),
        "an empty value must take no space");
    static_assert(alignof(DictEntry) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    // Keys are at most 512 MiB (proto-max-bulk-len, which the RDB loader
    // also applies), so their length fits the 32-bit field.
    void* const memory = ::operator new(sizeof(DictEntry) + key.size());
    auto* const entry = new (memory) DictEntry(
        static_cast<EntryHash>(hash), static_cast<uint32_t>(key.size()));
    if (!key.empty()) {
      std::memcpy(entry->KeyBytes(), key.data(), key.size());
    }
    return entry;
  }
  static void Destroy(DictEntry* entry) {
    entry->~DictEntry();
    ::operator delete(entry);
  }
  KeyView Key() const { return {KeyBytes(), key_length}; }

  // Takes no space when V is empty (NoValue).
  [[no_unique_address]] V value{};
  DictEntry* next{nullptr};
  EntryHash hash;
  uint32_t key_length;

 private:
  DictEntry(EntryHash key_hash, uint32_t length)
      : hash(key_hash), key_length(length) {}
  ~DictEntry() = default;
  char* KeyBytes() { return reinterpret_cast<char*>(this + 1); }
  const char* KeyBytes() const {
    return reinterpret_cast<const char*>(this + 1);
  }
};
}  // namespace dict_internal

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
  // How a stored key is read back: a std::string_view of a string key, whose
  // bytes live in its entry, and a const K& of any other.
  using KeyView = typename dict_internal::DictEntry<K, V>::KeyView;
  class EntryRef;
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
  // FindOrInsert that also returns a handle to the entry, so a caller can
  // refer to the stored key without looking it up again.
  struct InsertResult {
    V* value;
    EntryRef entry;
    bool inserted;
  };
  template <typename Lookup>
  InsertResult FindOrInsertEntry(const Lookup& key);
  // A handle to `key`'s entry, empty (false) when the key is absent.
  template <typename Lookup>
  EntryRef FindEntryRef(const Lookup& key);
  void Set(const K& key, const V& value);
  void Set(const K& key, V&& value);
  void Set(K&& key, V&& value);
  bool Insert(const K& key, const V& value);
  bool Insert(K&& key, V&& value);
  bool Delete(const K& key) { return DeleteImpl(key); }
  bool Delete(std::string_view key) { return DeleteImpl(key); }
  bool Delete(const char* key) { return Delete(std::string_view(key)); }
  bool Delete(const HashedKey& key) { return DeleteImpl(key); }
  // Delete that calls `before_delete(EntryRef)` with the entry it found,
  // once the entry is out of the table and before it is freed, so a caller
  // indexing entries elsewhere can drop it there without a second lookup.
  template <typename Lookup, typename Callback>
  bool DeleteWith(const Lookup& key, Callback&& before_delete);
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
  // Grows the table for `count` entries at once, as Redis' dictExpand does,
  // so inserting up to that many never stops to grow it. An empty dict gets
  // the table directly. One with entries moves them to it step by step, and
  // shrinks again if it is still mostly empty when they have moved, so
  // reserve before filling. Does nothing if the table is already that large
  // or is being rehashed.
  void Reserve(size_t count);
  void Clear();
  // Frees about `max_entries` entries, whole buckets at a time, so a large
  // Dict being discarded can be freed in steps between other work. Returns
  // true once it is empty. Between the first call and the one that returns
  // true, use the Dict only through this, Size(), and destruction.
  bool ClearSome(size_t max_entries);
  // Whether entries are still moving to a resized table. Both tables stay
  // allocated until the last one has moved.
  bool IsRehashing() const { return rehash_index_.has_value(); }
  // Moves up to `max_buckets` buckets of a rehash under way to the new
  // table. Operations on the Dict each move one bucket; this lets an idle
  // owner finish the rehash and release the old table. Returns whether more
  // can be moved now: false once the rehash is done, and while a scan, an
  // iterator, or the resize policy holds it back.
  bool RehashSome(int max_buckets);
  // Starts shrinking a table with fewer than an eighth of its buckets in
  // use. Deletes call this themselves; an owner calls it to retry a shrink
  // the resize policy held back when the entries were deleted.
  void ShrinkIfNeeded();
  ~Dict();

 private:
  using DictEntry = dict_internal::DictEntry<K, V>;
  struct EntryDeleter {
    void operator()(DictEntry* entry) const { DictEntry::Destroy(entry); }
  };
  using EntryPtr = std::unique_ptr<DictEntry, EntryDeleter>;

  // Four buckets, Redis' smallest table (DICT_HT_INITIAL_SIZE).
  static constexpr int kTableInitExp = 2;
  static constexpr size_t kTableInitSize = size_t{1} << kTableInitExp;
  // Shrink once fewer than 1/kShrinkRatio of the buckets are in use.
  static constexpr size_t kShrinkRatio = 8;
  // Under DictResizePolicy::kAvoid, a table still grows once it holds this
  // many entries per bucket, and shrinks once it is this many times emptier
  // than kShrinkRatio asks (Redis' dict_force_resize_ratio).
  static constexpr size_t kForceResizeRatio = 4;

  Dict();
  template <typename Lookup>
  V* FindValueImpl(const Lookup& key);
  template <typename Lookup>
  bool DeleteImpl(const Lookup& key);
  template <typename Lookup>
  std::optional<V> ExtractImpl(const Lookup& key);
  template <typename Lookup>
  DictEntry* FindEntry(const Lookup& key, size_t hash) const;
  // The entry for `key` among `table`'s buckets, which must not be empty.
  template <typename Lookup>
  DictEntry* FindInTable(int table, const Lookup& key, size_t hash) const;
  template <typename Lookup>
  DictEntry* Unlink(const Lookup& key);
  template <typename Lookup>
  DictEntry* InsertNew(const Lookup& key, size_t hash);
  // What a lookup key is copied from when it becomes a stored key.
  static std::string_view KeyOf(const HashedKey& key) { return key.key; }
  template <typename Lookup>
  static const Lookup& KeyOf(const Lookup& key) {
    return key;
  }
  // Links a new entry for `key`, which must not be present, with a
  // value-initialized value, and returns it.
  template <typename Source>
  DictEntry* AddEntry(Source&& key, size_t hash);
  template <typename Lookup>
  bool KeyEquals(const Lookup& key, const DictEntry* entry) const;
  template <typename Lookup>
  size_t KeyHash(const Lookup& key) const;
  void InsertEntry(DictEntry* entry, int table);
  size_t TableSize(int table) const { return tables_[table].size(); }
  size_t TableMask(int table) const {
    return tables_[table].empty() ? 0 : tables_[table].size() - 1;
  }
  void PauseRehashing() const { ++pause_rehash_; }
  void ResumeRehashing() const {
    if (pause_rehash_ > 0) {
      --pause_rehash_;
    }
  }
  void ExpandIfNeeded();
  bool Resize(size_t size);
  void RehashStepIfNeeded();
  bool Rehash(int n);
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
  // How many scans and iterators need entries to stay in their table.
  // Mutable so that reading through a const Dict can hold a rehash back.
  mutable size_t pause_rehash_{};
};

// A handle to a stored entry: its key, hash, and value, held without a copy
// of the key, so another container can index the same entries. The
// database's TTL index holds keyspace entries this way, as Redis' expires
// dict shares the keyspace's. Valid until the entry is deleted or the dict
// cleared; rehashing relinks entries without moving them.
template <typename K, typename V, typename Hash, typename KeyEqual>
class Dict<K, V, Hash, KeyEqual>::EntryRef {
 public:
  // An empty handle.
  EntryRef() = default;
  // Whether this refers to an entry; FindEntryRef returns an empty handle
  // for a missing key. The accessors below need a non-empty one.
  explicit operator bool() const { return entry_ != nullptr; }
  KeyView Key() const { return entry_->Key(); }
  size_t StoredHash() const { return entry_->hash; }
  V& Value() const { return entry_->value; }
  // The key with its stored hash, so looking it up does not hash it again.
  HashedKey Hashed() const
    requires std::same_as<Hash, DictHash<std::string>>
  {
    return HashedKey(entry_->Key(), entry_->hash);
  }
  bool operator==(const EntryRef& other) const = default;

 private:
  friend class Dict;
  explicit EntryRef(DictEntry* entry) : entry_(entry) {}
  DictEntry* entry_{};
};

// Walks every entry once. While an iterator lives the dict moves no entry
// between its tables (as Redis' safe iterators arrange), so the dict may be
// looked up, and other keys inserted, during the walk; inserted keys may or
// may not be visited. Deleting entries or clearing the dict invalidates it.
template <typename K, typename V, typename Hash, typename KeyEqual>
class Dict<K, V, Hash, KeyEqual>::Iterator {
 public:
  explicit Iterator(const Dict* dict);
  Iterator(const Iterator& other);
  Iterator& operator=(const Iterator& other);
  ~Iterator() { dict_->ResumeRehashing(); }
  bool operator==(const Iterator& other) const = default;
  bool Valid() const;
  void SeekToFirst();
  void SeekToLast();
  void Next();
  KeyView Key() const { return entry_->Key(); }
  const V& Value() const { return entry_->value; }
  // The current key with the hash stored in its entry, so it can be looked
  // up in other Dicts that share the seeded string hash without rehashing.
  HashedKey Hashed() const
    requires std::same_as<Hash, DictHash<std::string>>
  {
    return HashedKey(entry_->Key(), entry_->hash);
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
    : dict_(dict), table_(0), index_(std::nullopt), entry_(nullptr) {
  dict_->PauseRehashing();
}

template <typename K, typename V, typename Hash, typename KeyEqual>
Dict<K, V, Hash, KeyEqual>::Iterator::Iterator(const Iterator& other)
    : dict_(other.dict_),
      table_(other.table_),
      index_(other.index_),
      entry_(other.entry_) {
  dict_->PauseRehashing();
}

template <typename K, typename V, typename Hash, typename KeyEqual>
typename Dict<K, V, Hash, KeyEqual>::Iterator&
Dict<K, V, Hash, KeyEqual>::Iterator::operator=(const Iterator& other) {
  // Pause the new dict before resuming the old one, in case they are the same.
  other.dict_->PauseRehashing();
  dict_->ResumeRehashing();
  dict_ = other.dict_;
  table_ = other.table_;
  index_ = other.index_;
  entry_ = other.entry_;
  return *this;
}

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
  // Find the next non-empty bucket.
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
typename Dict<K, V, Hash, KeyEqual>::EntryRef
Dict<K, V, Hash, KeyEqual>::FindEntryRef(const Lookup& key) {
  if (Size() == 0) {
    return EntryRef(nullptr);
  }
  RehashStepIfNeeded();
  return EntryRef(FindEntry(key, KeyHash(key)));
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
V* Dict<K, V, Hash, KeyEqual>::FindValueImpl(const Lookup& key) {
  // Like Redis' dictFind, an empty dict answers without hashing the key,
  // which keeps lookups in an empty WATCH table nearly free.
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
  const InsertResult result = FindOrInsertEntry(key);
  return {result.value, result.inserted};
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
typename Dict<K, V, Hash, KeyEqual>::InsertResult
Dict<K, V, Hash, KeyEqual>::FindOrInsertEntry(const Lookup& key) {
  ExpandIfNeeded();
  RehashStepIfNeeded();
  const size_t hash = KeyHash(key);
  if (DictEntry* const existing = FindEntry(key, hash); existing != nullptr) {
    return {.value = &existing->value,
            .entry = EntryRef(existing),
            .inserted = false};
  }
  DictEntry* const entry = InsertNew(key, hash);
  return {.value = &entry->value, .entry = EntryRef(entry), .inserted = true};
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
  DictEntry* entry = FindEntry(key, hash);
  if (entry == nullptr) {
    entry = AddEntry(std::move(key), hash);
  }
  entry->value = std::move(value);
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
  AddEntry(std::move(key), hash)->value = std::move(value);
  return true;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
bool Dict<K, V, Hash, KeyEqual>::DeleteImpl(const Lookup& key) {
  EntryPtr entry(Unlink(key));
  if (entry == nullptr) {
    return false;
  }
  entry.reset();
  ShrinkIfNeeded();
  return true;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup, typename Callback>
bool Dict<K, V, Hash, KeyEqual>::DeleteWith(const Lookup& key,
                                            Callback&& before_delete) {
  EntryPtr entry(Unlink(key));
  if (entry == nullptr) {
    return false;
  }
  before_delete(EntryRef(entry.get()));
  entry.reset();
  ShrinkIfNeeded();
  return true;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
std::optional<V> Dict<K, V, Hash, KeyEqual>::ExtractImpl(const Lookup& key) {
  EntryPtr entry(Unlink(key));
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
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::ClearSome(size_t max_entries) {
  for (const int table : {1, 0}) {
    std::vector<DictEntry*>& buckets = tables_[table];
    while (!buckets.empty()) {
      if (max_entries == 0) {
        return false;
      }
      DictEntry* entry = buckets.back();
      buckets.pop_back();
      while (entry != nullptr) {
        DictEntry* const next = entry->next;
        DictEntry::Destroy(entry);
        --table_used_[table];
        max_entries -= max_entries > 0 ? 1 : 0;
        entry = next;
      }
    }
    std::vector<DictEntry*>().swap(buckets);
  }
  rehash_index_ = std::nullopt;
  return true;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
size_t Dict<K, V, Hash, KeyEqual>::KeyHash(const Lookup& key) const {
  return static_cast<dict_internal::EntryHash>(hash_(key));
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
bool Dict<K, V, Hash, KeyEqual>::KeyEquals(const Lookup& key,
                                           const DictEntry* entry) const {
  return key_equal_(key, entry->Key());
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
typename Dict<K, V, Hash, KeyEqual>::DictEntry*
Dict<K, V, Hash, KeyEqual>::FindEntry(const Lookup& key, size_t hash) const {
  // Callers make sure table 0 has buckets: lookups return early for an
  // empty dict, and inserts expand first. Table 1 has them while rehashing.
  if (DictEntry* const entry = FindInTable(0, key, hash); entry != nullptr) {
    return entry;
  }
  return IsRehashing() ? FindInTable(1, key, hash) : nullptr;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Lookup>
typename Dict<K, V, Hash, KeyEqual>::DictEntry*
Dict<K, V, Hash, KeyEqual>::FindInTable(int table, const Lookup& key,
                                        size_t hash) const {
  const std::vector<DictEntry*>& buckets = tables_[table];
  for (DictEntry* entry = buckets[hash & (buckets.size() - 1)];
       entry != nullptr; entry = entry->next) {
    if (entry->hash == hash && KeyEquals(key, entry)) {
      return entry;
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
    // `link` is whatever points at the current entry: the bucket, or the
    // entry before it. Unlinking is then one assignment in either case.
    for (DictEntry** link = &tables_[table][hash & TableMask(table)];
         *link != nullptr; link = &(*link)->next) {
      DictEntry* const entry = *link;
      if (entry->hash == hash && KeyEquals(key, entry)) {
        *link = entry->next;
        entry->next = nullptr;
        --table_used_[table];
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
  return AddEntry(KeyOf(key), hash);
}

template <typename K, typename V, typename Hash, typename KeyEqual>
template <typename Source>
typename Dict<K, V, Hash, KeyEqual>::DictEntry*
Dict<K, V, Hash, KeyEqual>::AddEntry(Source&& key, size_t hash) {
  DictEntry* const entry = DictEntry::Create(std::forward<Source>(key), hash);
  InsertEntry(entry, IsRehashing() ? 1 : 0);
  return entry;
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::InsertEntry(DictEntry* entry, int table) {
  DictEntry*& bucket = tables_[table][entry->hash & TableMask(table)];
  entry->next = bucket;
  bucket = entry;
  ++table_used_[table];
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::ExpandIfNeeded() {
  // Keep chains short: grow once there is one entry per bucket.
  if (IsRehashing() || table_used_[0] < TableSize(0)) {
    return;
  }
  const DictResizePolicy policy = CurrentDictResizePolicy();
  // A dict without buckets always gets its first table.
  if (policy == DictResizePolicy::kEnabled || tables_[0].empty() ||
      (policy == DictResizePolicy::kAvoid &&
       table_used_[0] >= kForceResizeRatio * TableSize(0))) {
    Resize(table_used_[0] + 1);
  }
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::ShrinkIfNeeded() {
  if (IsRehashing() || pause_rehash_ != 0 || TableSize(0) <= kTableInitSize ||
      table_used_[0] * kShrinkRatio > TableSize(0)) {
    return;
  }
  const DictResizePolicy policy = CurrentDictResizePolicy();
  if (policy == DictResizePolicy::kEnabled ||
      (policy == DictResizePolicy::kAvoid &&
       table_used_[0] * kShrinkRatio * kForceResizeRatio <= TableSize(0))) {
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
void Dict<K, V, Hash, KeyEqual>::Reserve(size_t count) {
  if (IsRehashing() || count <= TableSize(0)) {
    return;
  }
  if (Size() > 0) {
    Resize(count);
    return;
  }
  // Nothing to move: replace the table instead of rehashing into a second.
  const size_t old_size = TableSize(0);
  tables_[0].clear();
  if (!Resize(count)) {
    Resize(old_size);
  }
}

template <typename K, typename V, typename Hash, typename KeyEqual>
void Dict<K, V, Hash, KeyEqual>::RehashStepIfNeeded() {
  if (pause_rehash_ == 0) {
    Rehash(1);
  }
}

template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::RehashSome(int max_buckets) {
  return pause_rehash_ == 0 && Rehash(max_buckets);
}

// Performs at most n non-empty bucket migrations. Empty buckets are skipped
// with a bounded visit count so sparse tables cannot stall one operation.
// Returns whether the rehash is still under way and free to continue.
template <typename K, typename V, typename Hash, typename KeyEqual>
bool Dict<K, V, Hash, KeyEqual>::Rehash(int n) {
  if (!IsRehashing()) {
    return false;
  }
  const size_t old_size = TableSize(0);
  if (const DictResizePolicy policy = CurrentDictResizePolicy();
      policy != DictResizePolicy::kEnabled) {
    // A rehash already under way waits for the child too, unless the new
    // table is so far from the old one's size that waiting costs more.
    const size_t new_size = TableSize(1);
    const bool far_apart =
        new_size > old_size
            ? new_size >= kForceResizeRatio * old_size
            : old_size >= kShrinkRatio * kForceResizeRatio * new_size;
    if (policy == DictResizePolicy::kForbidden || !far_apart) {
      return false;
    }
  }
  int empty_visits = n * 10;
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
    return false;
  }
  return true;
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
      DictEntry::Destroy(entry);
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
    if constexpr (std::invocable<Visitor&, KeyView, const V&>) {
      visitor(entry->Key(), entry->value);
    } else {
      // A visitor that takes a HashedKey gets the hash stored with the
      // entry, so it can look the key up in another Dict without rehashing.
      visitor(HashedKey(entry->Key(), entry->hash), entry->value);
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
