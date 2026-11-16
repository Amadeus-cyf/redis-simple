#ifndef REDIS_SIMPLE_CONTAINERS_HANDLE_SET_H_
#define REDIS_SIMPLE_CONTAINERS_HANDLE_SET_H_

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace redis_simple::containers {
// A set of handles to objects stored elsewhere, such as Dict::EntryRef. A
// handle is one pointer wide and its default value means "none"; the set
// keeps each handle in an 8-byte slot of an open-addressed table and
// allocates nothing per member. The database's TTL index is one, as Redis'
// expires dict stores a pointer to each key's object in the bucket itself.
//
// Looking a handle up compares slots and never reads what it refers to.
// `Hash` is called only to place a handle: when it is inserted or erased,
// and when a table is rebuilt.
//
// A table is rebuilt into a new one when it fills up or empties, a few
// slots per insert or erase, so no single call moves every member.
template <typename Handle, typename Hash>
class HandleSet {
  static_assert(std::is_trivially_copyable_v<Handle> &&
                    sizeof(Handle) == sizeof(uintptr_t),
                "a handle must be a trivially copyable pointer wrapper");
  static_assert(std::copy_constructible<Hash> &&
                    std::is_invocable_r_v<size_t, const Hash&, const Handle&>,
                "Hash must map a handle to a size_t hash");

 public:
  HandleSet() = default;
  HandleSet(const HandleSet&) = delete;
  HandleSet& operator=(const HandleSet&) = delete;
  // Returns false, changing nothing, if `handle` is already a member. The
  // handle must not be the default one.
  bool Insert(Handle handle);
  // Returns whether `handle` was a member.
  bool Erase(Handle handle);
  bool Contains(Handle handle) const;
  size_t Size() const { return main_.used + old_.used; }
  void Clear();
  // Calls `visitor(handle)` for the members at one table position and
  // returns the next cursor, or nullopt after the last position. A sweep
  // from cursor 0 visits every member that stays in the set throughout,
  // unless a table is being rebuilt meanwhile: then a member may be visited
  // twice or not at all. The visitor must not change the set.
  template <typename Visitor>
  std::optional<size_t> Scan(size_t cursor, Visitor&& visitor) const;

 private:
  // A slot holds a handle's bits, kEmpty, or kErased. No handle has kErased's
  // bits, since the objects handles point to are aligned.
  static constexpr uintptr_t kEmpty = 0;
  static constexpr uintptr_t kErased = 1;
  static constexpr size_t kInitialCapacity = 16;
  static constexpr size_t kSlotsMovedPerCall = 16;

  struct Table {
    std::unique_ptr<uintptr_t[]> slots;
    // Zero or a power of two.
    size_t capacity{};
    size_t used{};
    // Erased slots still count against the table's capacity until it is
    // rebuilt, because a search has to walk past them.
    size_t erased{};
  };

  static uintptr_t Bits(Handle handle) {
    return std::bit_cast<uintptr_t>(handle);
  }
  static bool Holds(uintptr_t slot) { return slot > kErased; }
  // The slot holding `bits`, or null. Probes linearly from the slot the
  // hash names until an empty one.
  static uintptr_t* Find(const Table& table, uintptr_t bits, size_t hash);
  // Stores a handle that is not in `table`, which must have an empty slot
  // to spare.
  static void Place(Table* table, uintptr_t bits, size_t hash);
  bool Rebuilding() const { return old_.capacity != 0; }
  // Makes main_ a new table sized for the current members and starts moving
  // them into it from old_.
  void StartRebuild();
  void MoveSome(size_t max_slots);
  void MoveOldSlot(size_t index);

  // Receives every insert.
  Table main_;
  // The table being emptied into main_ while a rebuild runs.
  Table old_;
  // Slots of old_ before this index have been moved.
  size_t move_index_{};
  [[no_unique_address]] Hash hash_{};
};

template <typename Handle, typename Hash>
uintptr_t* HandleSet<Handle, Hash>::Find(const Table& table, uintptr_t bits,
                                         size_t hash) {
  if (table.capacity == 0) {
    return nullptr;
  }
  const size_t mask = table.capacity - 1;
  for (size_t index = hash & mask;; index = (index + 1) & mask) {
    uintptr_t* const slot = &table.slots[index];
    if (*slot == bits) {
      return slot;
    }
    if (*slot == kEmpty) {
      return nullptr;
    }
  }
}

template <typename Handle, typename Hash>
void HandleSet<Handle, Hash>::Place(Table* table, uintptr_t bits, size_t hash) {
  const size_t mask = table->capacity - 1;
  size_t index = hash & mask;
  while (Holds(table->slots[index])) {
    index = (index + 1) & mask;
  }
  if (table->slots[index] == kErased) {
    --table->erased;
  }
  table->slots[index] = bits;
  ++table->used;
}

template <typename Handle, typename Hash>
bool HandleSet<Handle, Hash>::Insert(Handle handle) {
  const uintptr_t bits = Bits(handle);
  const size_t hash = hash_(handle);
  MoveSome(kSlotsMovedPerCall);
  if (Find(main_, bits, hash) != nullptr || Find(old_, bits, hash) != nullptr) {
    return false;
  }
  // Every member ends up in main_, so its load counts those still in old_.
  // Three quarters full is where linear probing starts to slow down.
  if ((Size() + main_.erased + 1) * 4 > main_.capacity * 3) {
    // A rebuild normally finishes long before main_ fills again; if it has
    // not, finish it now, since only one can run at a time.
    MoveSome(old_.capacity);
    StartRebuild();
  }
  Place(&main_, bits, hash);
  return true;
}

template <typename Handle, typename Hash>
bool HandleSet<Handle, Hash>::Erase(Handle handle) {
  const uintptr_t bits = Bits(handle);
  const size_t hash = hash_(handle);
  MoveSome(kSlotsMovedPerCall);
  if (uintptr_t* const slot = Find(main_, bits, hash)) {
    *slot = kErased;
    --main_.used;
    ++main_.erased;
  } else if (uintptr_t* const old_slot = Find(old_, bits, hash)) {
    *old_slot = kErased;
    --old_.used;
  } else {
    return false;
  }
  // Shrink a table left mostly empty, so a scan does not walk long runs of
  // empty slots.
  if (!Rebuilding() && main_.capacity > kInitialCapacity &&
      Size() * 8 < main_.capacity) {
    StartRebuild();
  }
  return true;
}

template <typename Handle, typename Hash>
bool HandleSet<Handle, Hash>::Contains(Handle handle) const {
  const uintptr_t bits = Bits(handle);
  const size_t hash = hash_(handle);
  return Find(main_, bits, hash) != nullptr ||
         Find(old_, bits, hash) != nullptr;
}

template <typename Handle, typename Hash>
void HandleSet<Handle, Hash>::Clear() {
  main_ = Table();
  old_ = Table();
  move_index_ = 0;
}

template <typename Handle, typename Hash>
template <typename Visitor>
std::optional<size_t> HandleSet<Handle, Hash>::Scan(size_t cursor,
                                                    Visitor&& visitor) const {
  for (const Table* const table : {&main_, &old_}) {
    if (cursor < table->capacity && Holds(table->slots[cursor])) {
      visitor(std::bit_cast<Handle>(table->slots[cursor]));
    }
  }
  const size_t next = cursor + 1;
  if (next < std::max(main_.capacity, old_.capacity)) {
    return next;
  }
  return std::nullopt;
}

template <typename Handle, typename Hash>
void HandleSet<Handle, Hash>::StartRebuild() {
  // Half full at most once every member has moved. At least a quarter of
  // the old capacity, so that main_ cannot fill up before the old table,
  // emptied kSlotsMovedPerCall slots at a time, is done.
  const size_t capacity = std::max(
      {kInitialCapacity, std::bit_ceil(2 * (Size() + 1)), main_.capacity / 4});
  old_ = std::exchange(main_,
                       Table{.slots = std::make_unique<uintptr_t[]>(capacity),
                             .capacity = capacity});
  move_index_ = 0;
  if (old_.used == 0) {
    old_ = Table();
  }
}

template <typename Handle, typename Hash>
void HandleSet<Handle, Hash>::MoveSome(size_t max_slots) {
  if (!Rebuilding()) {
    return;
  }
  const size_t end = std::min(old_.capacity, move_index_ + max_slots);
  for (; move_index_ < end; ++move_index_) {
    MoveOldSlot(move_index_);
  }
  if (old_.used == 0) {
    old_ = Table();
    move_index_ = 0;
  }
}

template <typename Handle, typename Hash>
void HandleSet<Handle, Hash>::MoveOldSlot(size_t index) {
  const uintptr_t bits = old_.slots[index];
  if (!Holds(bits)) {
    return;
  }
  // The slot is marked erased rather than emptied, so searches for members
  // still in old_ keep walking past it.
  old_.slots[index] = kErased;
  --old_.used;
  Place(&main_, bits, hash_(std::bit_cast<Handle>(bits)));
}
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_HANDLE_SET_H_
