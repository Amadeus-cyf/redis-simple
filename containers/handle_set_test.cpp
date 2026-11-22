#include "containers/handle_set.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace redis_simple::containers {
namespace {
struct Item {
  size_t hash;
};

// A handle as the database uses one: a pointer to an object that carries its
// own hash.
struct ItemRef {
  Item* item{};
  bool operator==(const ItemRef&) const = default;
  bool operator<(const ItemRef& other) const { return item < other.item; }
};

struct ItemRefHash {
  size_t operator()(const ItemRef& ref) const { return ref.item->hash; }
};

using ItemSet = HandleSet<ItemRef, ItemRefHash>;

std::vector<std::unique_ptr<Item>> MakeItems(size_t count,
                                             size_t distinct_hashes) {
  std::vector<std::unique_ptr<Item>> items;
  items.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    // Hashes spread like real ones, repeated every `distinct_hashes` items
    // so that many members compete for the same slots.
    items.push_back(std::make_unique<Item>(
        Item{.hash = (index % distinct_hashes) * 0x9E3779B97F4A7C15ULL}));
  }
  return items;
}

std::set<ItemRef> Members(const ItemSet& set) {
  std::set<ItemRef> members;
  std::optional<size_t> cursor = 0;
  while (cursor.has_value()) {
    cursor = set.Scan(*cursor, [&members](ItemRef ref) {
      EXPECT_TRUE(members.insert(ref).second) << "member visited twice";
    });
  }
  return members;
}

size_t Positions(const ItemSet& set) {
  size_t positions = 0;
  for (std::optional<size_t> cursor = 0; cursor.has_value(); ++positions) {
    cursor = set.Scan(*cursor, [](ItemRef) {});
  }
  return positions;
}
}  // namespace

TEST(HandleSetTest, DoesNotShrinkWhileResizingIsAvoided) {
  constexpr size_t kCount = 1000;
  const auto items = MakeItems(kCount, kCount);
  ItemSet set;
  for (const auto& item : items) {
    ASSERT_TRUE(set.Insert({item.get()}));
  }
  const size_t positions = Positions(set);

  SetDictResizePolicy(DictResizePolicy::kAvoid);
  for (size_t index = 2; index < kCount; ++index) {
    ASSERT_TRUE(set.Delete({items[index].get()}));
  }
  EXPECT_EQ(Positions(set), positions);

  SetDictResizePolicy(DictResizePolicy::kEnabled);
  // The first erase starts the rebuild and the rest move the old table's
  // slots, a few per call.
  for (size_t index = 0; index < positions; ++index) {
    set.Insert({items[1].get()});
    set.Delete({items[1].get()});
  }
  EXPECT_LT(Positions(set), positions);
  EXPECT_TRUE(set.Contains({items[0].get()}));
}

TEST(HandleSetTest, ShrinkIfNeededStartsAShrinkThatRebuildSomeFinishes) {
  constexpr size_t kCount = 1000;
  const auto items = MakeItems(kCount, kCount);
  ItemSet set;
  for (const auto& item : items) {
    ASSERT_TRUE(set.Insert({item.get()}));
  }
  while (set.RebuildSome(64)) {
  }
  const size_t positions = Positions(set);
  SetDictResizePolicy(DictResizePolicy::kAvoid);
  for (size_t index = 2; index < kCount; ++index) {
    ASSERT_TRUE(set.Delete({items[index].get()}));
  }
  set.ShrinkIfNeeded();
  EXPECT_FALSE(set.IsRebuilding());

  SetDictResizePolicy(DictResizePolicy::kEnabled);
  set.ShrinkIfNeeded();
  ASSERT_TRUE(set.IsRebuilding());
  EXPECT_TRUE(set.RebuildSome(1));
  while (set.RebuildSome(64)) {
  }
  EXPECT_LT(Positions(set), positions);
  EXPECT_EQ(set.Size(), 2);
  EXPECT_TRUE(set.Contains({items[0].get()}));
  EXPECT_TRUE(set.Contains({items[1].get()}));
}

TEST(HandleSetTest, ReserveMakesRoomSoInsertsDoNotRebuild) {
  constexpr size_t kCount = 1000;
  const auto items = MakeItems(kCount, kCount);
  ItemSet set;
  ASSERT_TRUE(set.Insert({items[0].get()}));
  set.Reserve(kCount);
  while (set.RebuildSome(64)) {
  }
  const size_t positions = Positions(set);
  for (size_t index = 1; index < kCount; ++index) {
    ASSERT_TRUE(set.Insert({items[index].get()}));
    ASSERT_FALSE(set.IsRebuilding());
  }
  EXPECT_EQ(Positions(set), positions);
  EXPECT_EQ(Members(set).size(), kCount);
}

TEST(HandleSetTest, InsertsDeletesAndFindsHandles) {
  const auto items = MakeItems(3, 3);
  ItemSet set;
  EXPECT_EQ(set.Size(), 0);
  EXPECT_FALSE(set.Contains({items[0].get()}));
  EXPECT_FALSE(set.Delete({items[0].get()}));
  EXPECT_EQ(set.Scan(0, [](ItemRef) { FAIL(); }), std::nullopt);

  EXPECT_TRUE(set.Insert({items[0].get()}));
  EXPECT_FALSE(set.Insert({items[0].get()}));
  EXPECT_TRUE(set.Insert({items[1].get()}));
  EXPECT_EQ(set.Size(), 2);
  EXPECT_TRUE(set.Contains({items[0].get()}));
  EXPECT_TRUE(set.Contains({items[1].get()}));
  EXPECT_FALSE(set.Contains({items[2].get()}));

  EXPECT_TRUE(set.Delete({items[0].get()}));
  EXPECT_FALSE(set.Delete({items[0].get()}));
  EXPECT_FALSE(set.Contains({items[0].get()}));
  EXPECT_TRUE(set.Contains({items[1].get()}));
  EXPECT_EQ(set.Size(), 1);

  set.Clear();
  EXPECT_EQ(set.Size(), 0);
  EXPECT_FALSE(set.Contains({items[1].get()}));
  EXPECT_TRUE(set.Insert({items[1].get()}));
}

TEST(HandleSetTest, KeepsEveryMemberWhileGrowingAndShrinking) {
  constexpr size_t kCount = 5000;
  // A few hundred distinct hashes, so probe runs are long and cross the
  // slots a rebuild has already moved.
  const auto items = MakeItems(kCount, 300);
  ItemSet set;
  std::set<ItemRef> expected;
  for (size_t index = 0; index < kCount; ++index) {
    ASSERT_TRUE(set.Insert({items[index].get()}));
    expected.insert({items[index].get()});
    if (index % 7 == 3) {
      // Erase an earlier member while tables are being rebuilt.
      const ItemRef earlier{items[index / 2].get()};
      ASSERT_EQ(set.Delete(earlier), expected.erase(earlier) == 1);
    }
  }
  EXPECT_EQ(set.Size(), expected.size());
  for (size_t index = 0; index < kCount; ++index) {
    const ItemRef ref{items[index].get()};
    EXPECT_EQ(set.Contains(ref), expected.contains(ref));
  }

  // Erase all but a few, which shrinks the table step by step.
  for (size_t index = 0; index < kCount; ++index) {
    const ItemRef ref{items[index].get()};
    if (index % 500 != 0) {
      ASSERT_EQ(set.Delete(ref), expected.erase(ref) == 1);
    }
  }
  EXPECT_EQ(set.Size(), expected.size());
  // Inserts and erases that change nothing let a running rebuild finish, so
  // one sweep sees each member once.
  for (size_t index = 0; index < kCount; ++index) {
    set.Delete({items[1].get()});
  }
  EXPECT_EQ(Members(set), expected);
  // The shrunken table holds few enough slots for a short sweep.
  size_t positions = 0;
  for (std::optional<size_t> cursor = 0; cursor.has_value();
       cursor = set.Scan(*cursor, [](ItemRef) {})) {
    ++positions;
  }
  EXPECT_LE(positions, 16 * (expected.size() + 1));
}

TEST(HandleSetTest, ReusesDeletedSlotsUnderChurn) {
  // The same few members inserted and erased many times must not grow the
  // table without bound or lose anyone.
  const auto items = MakeItems(64, 8);
  ItemSet set;
  std::set<ItemRef> expected;
  uint64_t state = 12345;
  for (int round = 0; round < 200'000; ++round) {
    state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
    const ItemRef ref{items[(state >> 33) % items.size()].get()};
    if ((state >> 20) % 2 == 0) {
      ASSERT_EQ(set.Insert(ref), expected.insert(ref).second);
    } else {
      ASSERT_EQ(set.Delete(ref), expected.erase(ref) == 1);
    }
    ASSERT_EQ(set.Size(), expected.size());
  }
  for (const auto& item : items) {
    EXPECT_EQ(set.Contains({item.get()}), expected.contains({item.get()}));
  }
  size_t positions = 0;
  for (std::optional<size_t> cursor = 0; cursor.has_value();
       cursor = set.Scan(*cursor, [](ItemRef) {})) {
    ++positions;
  }
  EXPECT_LE(positions, 1024);
}
}  // namespace redis_simple::containers
