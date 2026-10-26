#include "memory/intset.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace redis_simple::in_memory {
namespace {
std::unique_ptr<IntSet> MakeFullIntSet() {
  auto intset = std::make_unique<IntSet>();
  intset->Add(1);
  intset->Add(5);
  intset->Add(7);
  intset->Add(3);
  intset->Add(2);
  intset->Add(-1);
  intset->Add(INT16_MIN);
  intset->Add(INT16_MAX);
  intset->Add(INT32_MIN);
  intset->Add(INT32_MAX);
  intset->Add(INT64_MIN);
  intset->Add(INT64_MAX);
  return intset;
}

std::unique_ptr<IntSet> MakeIntSetAfterRemovals() {
  auto intset = MakeFullIntSet();
  intset->Remove(INT16_MAX);
  intset->Remove(INT64_MIN);
  intset->Remove(INT64_MAX);
  return intset;
}
}  // namespace

TEST(IntSetTest, AddAndUpgradeAndGet) {
  auto intset = MakeFullIntSet();
  ASSERT_FALSE(intset->Add(1));
  ASSERT_EQ(intset->Size(), 12);
  ASSERT_FALSE(intset->Add(INT64_MIN));
  ASSERT_EQ(intset->Size(), 12);
  ASSERT_FALSE(intset->Add(INT64_MAX));
  ASSERT_EQ(intset->Size(), 12);

  ASSERT_EQ(intset->Get(0), INT64_MIN);
  ASSERT_EQ(intset->Get(1), INT32_MIN);
  ASSERT_EQ(intset->Get(2), INT16_MIN);
  ASSERT_EQ(intset->Get(3), -1);
  ASSERT_EQ(intset->Get(4), 1);
  ASSERT_EQ(intset->Get(5), 2);
  ASSERT_EQ(intset->Get(6), 3);
  ASSERT_EQ(intset->Get(7), 5);
  ASSERT_EQ(intset->Get(8), 7);
  ASSERT_EQ(intset->Get(9), INT16_MAX);
  ASSERT_EQ(intset->Get(10), INT32_MAX);
  ASSERT_EQ(intset->Get(11), INT64_MAX);
  EXPECT_DEATH(intset->Get(-1), "Check failed");
  EXPECT_DEATH(intset->Get(12), "Check failed");
}

TEST(IntSetTest, Find) {
  auto intset = MakeFullIntSet();
  ASSERT_TRUE(intset->Find(INT64_MIN));
  ASSERT_TRUE(intset->Find(INT64_MAX));
  ASSERT_TRUE(intset->Find(INT16_MIN));
  ASSERT_TRUE(intset->Find(INT16_MAX));
  ASSERT_TRUE(intset->Find(-1));
  ASSERT_TRUE(intset->Find(7));
  ASSERT_FALSE(intset->Find(-7));
  ASSERT_FALSE(intset->Find(0));
  ASSERT_FALSE(intset->Find(INT64_MIN + 1));
  ASSERT_FALSE(intset->Find(INT64_MAX - 1));
  ASSERT_FALSE(intset->Find(INT32_MIN + 1));
  ASSERT_FALSE(intset->Find(INT32_MAX - 1));
}

TEST(IntSetTest, Remove) {
  auto intset = MakeFullIntSet();
  ASSERT_TRUE(intset->Remove(INT16_MAX));
  ASSERT_FALSE(intset->Find(INT16_MAX));
  ASSERT_EQ(intset->Size(), 11);
  ASSERT_FALSE(intset->Remove(INT16_MAX));
  ASSERT_EQ(intset->Size(), 11);

  ASSERT_TRUE(intset->Remove(INT64_MIN));
  ASSERT_FALSE(intset->Find(INT64_MIN));
  ASSERT_EQ(intset->Size(), 10);
  ASSERT_FALSE(intset->Remove(INT64_MIN));
  ASSERT_EQ(intset->Size(), 10);

  ASSERT_TRUE(intset->Remove(INT64_MAX));
  ASSERT_FALSE(intset->Find(INT64_MAX));
  ASSERT_EQ(intset->Size(), 9);
  ASSERT_FALSE(intset->Remove(INT64_MAX));
  ASSERT_EQ(intset->Size(), 9);
}

TEST(IntSetTest, Iterator) {
  auto intset = MakeIntSetAfterRemovals();
  auto it = IntSet::Iterator(intset.get());
  it.SeekToFirst();
  ASSERT_EQ(it.Value(), INT32_MIN);
  it.Next();
  ASSERT_EQ(it.Value(), INT16_MIN);
  it.Next();
  ASSERT_EQ(it.Value(), -1);
  it.Next();
  ASSERT_EQ(it.Value(), 1);
  it.Next();
  ASSERT_EQ(it.Value(), 2);
  it.Next();
  ASSERT_EQ(it.Value(), 3);
  it.Next();
  ASSERT_EQ(it.Value(), 5);
  it.Next();
  ASSERT_EQ(it.Value(), 7);
  it.SeekToLast();
  ASSERT_EQ(it.Value(), INT32_MAX);
}
}  // namespace redis_simple::in_memory
