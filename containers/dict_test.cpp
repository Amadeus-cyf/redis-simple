#include "containers/dict.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redis_simple::containers {
TEST(DictStringTest, Basics) {
  auto string_dict = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(string_dict);
  ASSERT_EQ(string_dict->Size(), 0);
}

namespace {
// Case-insensitive policies exercise the hash and equality parameters.
struct CaseInsensitiveHash {
  size_t operator()(std::string_view key) const {
    size_t hash = 0;
    for (const char ch : key) {
      hash = (hash * 31) + static_cast<unsigned char>(std::tolower(ch));
    }
    return hash;
  }
};

struct CaseInsensitiveEqual {
  bool operator()(std::string_view left, std::string_view right) const {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char a, char b) {
                        return std::tolower(a) == std::tolower(b);
                      });
  }
};
}  // namespace

TEST(DictStringTest, UsesCustomHashAndEqualityPolicies) {
  auto dict = Dict<std::string, int, CaseInsensitiveHash,
                   CaseInsensitiveEqual>::Create();
  ASSERT_TRUE(dict->Insert("Key", 1));
  EXPECT_FALSE(dict->Insert("KEY", 2));
  ASSERT_NE(dict->FindValue(std::string_view("kEy")), nullptr);
  EXPECT_EQ(*dict->FindValue(std::string_view("kEy")), 1);
}

namespace {
// Puts every key in one bucket with one stored hash, so each lookup has to
// tell the keys apart by comparing them.
struct CollidingHash {
  size_t operator()(std::string_view /*key*/) const { return 7; }
};
}  // namespace

TEST(DictStringTest, DefaultEqualityTellsCollidingKeysApartByLengthAndBytes) {
  auto dict = Dict<std::string, size_t, CollidingHash>::Create();
  const std::vector<std::string> keys = {"",
                                         "a",
                                         "ab",
                                         "ac",
                                         std::string("nul\0a", 5),
                                         std::string("nul\0b", 5),
                                         std::string(64, 'k'),
                                         std::string(63, 'k') + "j"};
  for (size_t i = 0; i < keys.size(); ++i) {
    ASSERT_TRUE(dict->Insert(keys[i], i)) << i;
  }
  ASSERT_EQ(dict->Size(), keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    EXPECT_FALSE(dict->Insert(keys[i], keys.size())) << i;
    const size_t* const value = dict->FindValue(std::string_view(keys[i]));
    ASSERT_NE(value, nullptr) << i;
    EXPECT_EQ(*value, i);
  }
  EXPECT_EQ(dict->FindValue(std::string_view("abc")), nullptr);
  EXPECT_EQ(dict->FindValue(std::string_view("nul")), nullptr);
  EXPECT_EQ(dict->FindValue(std::string_view(std::string(65, 'k'))), nullptr);
}

TEST(DictStringTest, ClearSomeFreesInStepsAcrossBothTables) {
  auto dict = Dict<std::string, int>::Create(1024);
  // The 1025th insert begins a rehash, so entries sit in both tables.
  for (int i = 0; i < 1025; ++i) {
    dict->Set("key:" + std::to_string(i), i);
  }
  size_t steps = 0;
  size_t previous_size = dict->Size();
  while (!dict->ClearSome(100)) {
    ++steps;
    EXPECT_LT(dict->Size(), previous_size);
    previous_size = dict->Size();
  }
  EXPECT_GE(steps, 1025 / 200);
  EXPECT_EQ(dict->Size(), 0);
  EXPECT_TRUE(dict->ClearSome(0));
}

TEST(DictStringTest, FindOrInsertHashesOnceAndKeepsExistingValues) {
  auto dict = Dict<std::string, std::string>::Create();
  auto [value, inserted] = dict->FindOrInsert(std::string_view("key"));
  ASSERT_TRUE(inserted);
  *value = "first";
  auto [same, inserted_again] = dict->FindOrInsert(std::string_view("key"));
  EXPECT_FALSE(inserted_again);
  EXPECT_EQ(same, value);
  EXPECT_EQ(*same, "first");
  EXPECT_EQ(dict->Size(), 1);
}

TEST(DictStringTest, HashedKeysMatchPlainLookupsAcrossDicts) {
  auto values = Dict<std::string, std::string>::Create();
  auto counts = Dict<std::string, int>::Create();
  // Enough keys to resize and rehash both tables while they are used.
  for (int index = 0; index < 200; ++index) {
    const std::string key = "key:" + std::to_string(index);
    const HashedKey hashed(key);
    EXPECT_EQ(hashed.hash, DictHash<std::string>{}(std::string_view(key)));
    auto [value, inserted] = values->FindOrInsert(hashed);
    ASSERT_TRUE(inserted);
    *value = key;
    *counts->FindOrInsert(hashed).first = index;
  }
  for (int index = 0; index < 200; ++index) {
    const std::string key = "key:" + std::to_string(index);
    const HashedKey hashed(key);
    ASSERT_EQ(values->FindValue(hashed), values->FindValue(key));
    ASSERT_NE(values->FindValue(hashed), nullptr);
    EXPECT_EQ(*values->FindValue(hashed), key);
    ASSERT_NE(counts->FindValue(std::string_view(key)), nullptr);
    EXPECT_EQ(*counts->FindValue(hashed), index);
  }
  EXPECT_EQ(values->FindValue(HashedKey("missing")), nullptr);

  EXPECT_EQ(values->Extract(HashedKey("key:7")),
            std::optional<std::string>("key:7"));
  EXPECT_TRUE(counts->Delete(HashedKey("key:7")));
  EXPECT_FALSE(counts->Delete(HashedKey("key:7")));
  EXPECT_EQ(values->FindValue(std::string_view("key:7")), nullptr);
  EXPECT_EQ(counts->FindValue(std::string_view("key:7")), nullptr);
  EXPECT_EQ(values->Size(), 199);
  EXPECT_EQ(counts->Size(), 199);
}

TEST(DictStringTest, IteratorExposesStoredHashes) {
  auto source = Dict<std::string, int>::Create();
  auto target = Dict<std::string, int>::Create();
  // Enough keys that the source rehashes while being filled.
  for (int index = 0; index < 300; ++index) {
    const std::string key = "member:" + std::to_string(index);
    ASSERT_TRUE(source->Insert(key, index));
    if (index % 3 == 0) {
      ASSERT_TRUE(target->Insert(key, index));
    }
  }
  size_t visited = 0;
  size_t found = 0;
  auto it = Dict<std::string, int>::Iterator(source.get());
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    const HashedKey hashed = it.Hashed();
    EXPECT_EQ(hashed.key, it.Key());
    EXPECT_EQ(hashed.hash, HashedKey(it.Key()).hash);
    found += target->FindValue(hashed) != nullptr ? 1 : 0;
    ++visited;
  }
  EXPECT_EQ(visited, 300);
  EXPECT_EQ(found, 100);
}

TEST(DictStringTest, ShrinksAfterMassDeletes) {
  auto dict = Dict<std::string, int>::Create();
  for (int i = 0; i < 4096; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  for (int i = 0; i < 4090; ++i) {
    ASSERT_TRUE(dict->Delete(std::string_view(std::to_string(i))));
  }
  // Finish any rehash still in flight; lookups drive incremental rehashing.
  for (int i = 0; i < 10000; ++i) {
    dict->FindValue(std::string_view("4095"));
  }
  int visited_buckets = 0;
  size_t cursor = 0;
  while (true) {
    const auto next = dict->Scan(cursor, [](std::string_view, const int&) {});
    ++visited_buckets;
    if (!next.has_value()) {
      break;
    }
    cursor = *next;
  }
  EXPECT_EQ(dict->Size(), 6);
  EXPECT_LE(visited_buckets, 64);
}

// Counts the buckets a full scan walks, which is the size of the table.
size_t CountBuckets(Dict<std::string, int>* dict) {
  size_t buckets = 0;
  size_t cursor = 0;
  while (true) {
    const auto next = dict->Scan(cursor, [](std::string_view, const int&) {});
    ++buckets;
    if (!next.has_value()) {
      return buckets;
    }
    cursor = *next;
  }
}

TEST(DictStringTest, ReserveSizesTheTableBeforeInserts) {
  auto dict = Dict<std::string, int>::Create();
  dict->Reserve(1000);
  EXPECT_EQ(dict->Size(), 0);
  // The table has its full size from the first insert on.
  dict->Insert("first", -1);
  EXPECT_EQ(CountBuckets(dict.get()), 1024);
  for (int i = 0; i < 999; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  EXPECT_EQ(dict->Size(), 1000);
  EXPECT_EQ(CountBuckets(dict.get()), 1024);
  EXPECT_EQ(*dict->FindValue(std::string_view("first")), -1);
  EXPECT_EQ(*dict->FindValue(std::string_view("998")), 998);

  // A smaller request leaves the table as it is.
  dict->Reserve(10);
  EXPECT_EQ(CountBuckets(dict.get()), 1024);

  // A count no table can hold leaves the dict usable.
  auto unchanged = Dict<std::string, int>::Create();
  unchanged->Reserve(std::numeric_limits<size_t>::max());
  unchanged->Insert("only", 1);
  EXPECT_EQ(CountBuckets(unchanged.get()), 4);
}

TEST(DictStringTest, ReserveKeepsTheEntriesOfAFilledDict) {
  auto dict = Dict<std::string, int>::Create();
  for (int i = 0; i < 100; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  dict->Reserve(5000);
  for (int i = 100; i < 5000; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  EXPECT_EQ(dict->Size(), 5000);
  for (int i = 0; i < 5000; ++i) {
    const int* value = dict->FindValue(std::string_view(std::to_string(i)));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, i);
  }
}

TEST(DictStringTest, ScanVisitsEveryKeyWhileTheTableResizes) {
  auto dict = Dict<std::string, int>::Create();
  for (int i = 0; i < 1000; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  std::vector<int> seen(2000, 0);
  size_t cursor = 0;
  int step = 0;
  while (true) {
    const std::optional<size_t> next =
        dict->Scan(cursor, [&seen](std::string_view, const int& v) {
          ++seen[static_cast<size_t>(v)];
        });
    if (!next.has_value()) {
      break;
    }
    cursor = *next;
    // Grow, then shrink, the table between cursor steps.
    if (step == 20) {
      for (int i = 1000; i < 2000; ++i) {
        dict->Insert(std::to_string(i), i);
      }
    } else if (step == 200) {
      for (int i = 1000; i < 2000; ++i) {
        dict->Delete(std::string_view(std::to_string(i)));
      }
    }
    ++step;
  }
  // Keys present for the whole scan must each be returned at least once.
  for (int i = 0; i < 1000; ++i) {
    EXPECT_GE(seen[static_cast<size_t>(i)], 1) << i;
  }
}

TEST(DictStringTest, KeysOfAnyLengthAndBytesAreStoredWithTheirEntries) {
  auto dict = Dict<std::string, std::string>::Create();
  const std::vector<std::string> keys = {"",
                                         "a",
                                         std::string("nul\0inside", 10),
                                         std::string(15, 'k'),
                                         std::string(16, 'k'),
                                         std::string(4096, 'k')};
  for (const std::string& key : keys) {
    dict->Set(key, "value of " + key);
  }
  ASSERT_EQ(dict->Size(), keys.size());
  for (const std::string& key : keys) {
    const std::string* const value = dict->FindValue(std::string_view(key));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, "value of " + key);
  }
  std::vector<std::string> stored;
  auto it = Dict<std::string, std::string>::Iterator(dict.get());
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    stored.emplace_back(it.Key());
    EXPECT_EQ(it.Value(), "value of " + stored.back());
  }
  std::vector<std::string> expected = keys;
  std::ranges::sort(stored);
  std::ranges::sort(expected);
  EXPECT_EQ(stored, expected);
}

TEST(DictStringTest, KeyViewsStayValidWhileTheTableGrows) {
  auto dict = Dict<std::string, int>::Create();
  dict->Set("first key, long enough to matter", 0);
  auto it = Dict<std::string, int>::Iterator(dict.get());
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  const std::string_view first = it.Key();
  for (int i = 1; i < 1000; ++i) {
    dict->Set("key" + std::to_string(i), i);
  }
  EXPECT_EQ(first, "first key, long enough to matter");
}

TEST(DictStringTest, Insert) {
  auto string_dict = Dict<std::string, std::string>::Create();
  bool status = string_dict->Insert("key", "val");
  ASSERT_TRUE(status);
  ASSERT_EQ(string_dict->Size(), 1);

  // Insert a new key.
  const auto opt1 = string_dict->Get("key");
  ASSERT_TRUE(opt1.has_value());
  ASSERT_EQ(opt1.value_or(""), "val");
  ASSERT_EQ(string_dict->Size(), 1);

  // Update the key.
  string_dict->Set("key", "val_update");
  ASSERT_EQ(string_dict->Size(), 1);

  const auto opt2 = string_dict->Get("key");
  ASSERT_TRUE(opt2.has_value());
  ASSERT_EQ(opt2.value_or(""), "val_update");
  ASSERT_EQ(string_dict->Size(), 1);
}

TEST(DictStringTest, InsertDuplicate) {
  auto string_dict = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(string_dict->Insert("key", "val"));
  ASSERT_FALSE(string_dict->Insert("key", "new_val"));
  ASSERT_EQ(string_dict->Size(), 1);

  const auto result = string_dict->Get("key");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value_or(""), "val");
}

TEST(DictStringTest, FindValueSupportsNonOwningLookup) {
  auto string_dict = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(string_dict->Insert("prefix_key_suffix", "val"));

  const std::string lookup = "xxprefix_key_suffixyy";
  auto* value = string_dict->FindValue(std::string_view(lookup).substr(2, 17));

  ASSERT_NE(value, nullptr);
  ASSERT_EQ(*value, "val");
  ASSERT_EQ(string_dict->FindValue(std::string_view("missing")), nullptr);
}

TEST(DictStringTest, Delete) {
  auto string_dict = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(string_dict->Insert("key", "val"));

  bool status = string_dict->Delete("key");
  ASSERT_TRUE(status);
  ASSERT_EQ(string_dict->Size(), 0);

  const auto result = string_dict->Get("key");
  ASSERT_FALSE(result.has_value());
  ASSERT_EQ(result, std::nullopt);
  ASSERT_EQ(string_dict->Size(), 0);
}

TEST(DictStringTest, ExtractTransfersMoveOnlyValue) {
  auto dict = Dict<std::string, std::unique_ptr<int>>::Create();
  dict->Set(std::string("key"), std::make_unique<int>(42));

  auto value = dict->Extract(std::string_view("key"));

  std::unique_ptr<int> extracted = std::move(value).value_or(nullptr);
  ASSERT_NE(extracted, nullptr);
  EXPECT_EQ(*extracted, 42);
  EXPECT_EQ(dict->Size(), 0);
  EXPECT_FALSE(dict->Extract(std::string_view("missing")).has_value());
}

namespace {
std::unique_ptr<Dict<int, int>> MakeIntDictWithEntries() {
  auto int_dict = Dict<int, int>::Create();
  for (int i = 0; i < 129; ++i) {
    int_dict->Insert(i, i);
  }
  return int_dict;
}
}  // namespace

TEST(DictIntTest, Basics) {
  auto int_dict = Dict<int, int>::Create();
  ASSERT_TRUE(int_dict);
  ASSERT_EQ(int_dict->Size(), 0);
}

TEST(DictIntTest, BatchInsert) {
  auto int_dict = MakeIntDictWithEntries();
  ASSERT_EQ(int_dict->Size(), 129);
  const std::optional<int> result = int_dict->Get(96);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value_or(0), 96);
}

TEST(DictIntTest, Iterator) {
  auto int_dict = MakeIntDictWithEntries();
  auto it = containers::Dict<int, int>::Iterator(int_dict.get());
  // Bucket order depends on the table size, so check coverage instead.
  std::vector<int> seen(129, 0);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    ASSERT_EQ(it.Key(), it.Value());
    ASSERT_GE(it.Key(), 0);
    ASSERT_LT(it.Key(), 129);
    ++seen[static_cast<size_t>(it.Key())];
  }
  EXPECT_EQ(std::count(seen.begin(), seen.end(), 1), 129);

  it.SeekToLast();
  ASSERT_TRUE(it.Valid());
  it.Next();
  ASSERT_FALSE(it.Valid());
}

TEST(DictIntTest, Clear) {
  auto int_dict = MakeIntDictWithEntries();
  int_dict->Clear();
  ASSERT_EQ(int_dict->Size(), 0);

  const std::optional<int> result = int_dict->Get(96);
  ASSERT_FALSE(result.has_value());
  ASSERT_EQ(result, std::nullopt);

  ASSERT_TRUE(int_dict->Insert(1, 10));
  ASSERT_EQ(int_dict->Size(), 1);
  ASSERT_EQ(int_dict->Get(1), 10);
}

TEST(DictStringTest, ScanStepVisitsCountElementsPerStep) {
  auto dict = Dict<std::string, int>::Create();
  EXPECT_EQ(dict->ScanStep(0, 10, [](std::string_view, const int&) {}), 0);
  for (int i = 0; i < 1000; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  std::vector<int> seen(1000, 0);
  size_t cursor = 0;
  while (true) {
    size_t visited = 0;
    cursor = dict->ScanStep(cursor, 10,
                            [&seen, &visited](std::string_view, const int& v) {
                              ++seen[static_cast<size_t>(v)];
                              ++visited;
                            });
    if (cursor == 0) {
      break;
    }
    // Like Redis, a step ends at a bucket boundary once it has visited
    // COUNT elements, unless the scan completes first.
    EXPECT_GE(visited, 10);
  }
  // Without changes during the scan, each key is visited exactly once.
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(seen[static_cast<size_t>(i)], 1) << i;
  }
}

namespace {
// Sets the process-wide resize policy for one test and restores it.
class ScopedResizePolicy {
 public:
  explicit ScopedResizePolicy(DictResizePolicy policy) {
    SetDictResizePolicy(policy);
  }
  ScopedResizePolicy(const ScopedResizePolicy&) = delete;
  ScopedResizePolicy& operator=(const ScopedResizePolicy&) = delete;
  ScopedResizePolicy(ScopedResizePolicy&&) = delete;
  ScopedResizePolicy& operator=(ScopedResizePolicy&&) = delete;
  ~ScopedResizePolicy() { SetDictResizePolicy(DictResizePolicy::kEnabled); }
};

// How many cursor positions a full scan takes: the bucket count, or the
// smaller table's while a rehash is under way.
size_t ScanPositions(Dict<int, int>* dict) {
  size_t positions = 0;
  std::optional<size_t> cursor = 0;
  while (cursor.has_value()) {
    cursor = dict->Scan(cursor.value_or(0), [](const int&, const int&) {});
    ++positions;
  }
  return positions;
}

void ExpectHolds(Dict<int, int>* dict, int count) {
  ASSERT_EQ(dict->Size(), static_cast<size_t>(count));
  for (int key = 0; key < count; ++key) {
    const int* const value = dict->FindValue(key);
    ASSERT_NE(value, nullptr) << key;
    EXPECT_EQ(*value, key);
  }
}
}  // namespace

TEST(DictIntTest, LookupsDuringIterationKeepEveryEntryVisitedOnce) {
  auto dict = Dict<int, int>::Create();
  // Enough inserts to leave a rehash under way, which lookups would advance.
  constexpr int kCount = 1000;
  for (int i = 0; i < kCount; ++i) {
    dict->Set(i, i);
  }
  std::vector<int> seen(kCount, 0);
  {
    auto it = Dict<int, int>::Iterator(dict.get());
    it.SeekToFirst();
    while (it.Valid()) {
      ++seen[it.Key()];
      for (int i = 0; i < 8; ++i) {
        ASSERT_NE(dict->FindValue(i), nullptr);
      }
      it.Next();
    }
  }
  EXPECT_EQ(std::ranges::count(seen, 1), kCount);
  // Once the iterator is gone, lookups finish the rehash again.
  for (int i = 0; i < 4 * kCount; ++i) {
    ASSERT_NE(dict->FindValue(i % kCount), nullptr);
  }
  EXPECT_EQ(ScanPositions(dict.get()), 1024U);
}

TEST(DictResizePolicyTest, AvoidGrowsOnlyAtFourEntriesPerBucket) {
  // While a forked child shares memory, as Redis' DICT_RESIZE_AVOID.
  const ScopedResizePolicy avoid(DictResizePolicy::kAvoid);
  auto dict = Dict<int, int>::Create(16);
  // 16 entries would grow the table normally; here 64 entries still fit.
  for (int key = 0; key < 64; ++key) {
    ASSERT_TRUE(dict->Insert(key, key));
  }
  EXPECT_EQ(ScanPositions(dict.get()), 16);
  ExpectHolds(dict.get(), 64);

  // The 65th finds four entries per bucket and grows. The new table is at
  // least four times the old, so the rehash runs despite the policy.
  ASSERT_TRUE(dict->Insert(64, 64));
  for (int round = 0; round < 4; ++round) {
    ExpectHolds(dict.get(), 65);
  }
  EXPECT_EQ(ScanPositions(dict.get()), 128);
}

TEST(DictResizePolicyTest, ForbiddenStopsARehashUnderWay) {
  auto dict = Dict<int, int>::Create(64);
  for (int key = 0; key < 65; ++key) {
    ASSERT_TRUE(dict->Insert(key, key));
  }
  // The 65th insert began a rehash from 64 buckets to 128.
  {
    // In a forked child, as Redis' DICT_RESIZE_FORBID: nothing moves, so
    // the child writes to none of the pages it shares.
    const ScopedResizePolicy forbidden(DictResizePolicy::kForbidden);
    for (int round = 0; round < 10; ++round) {
      ExpectHolds(dict.get(), 65);
    }
    EXPECT_EQ(ScanPositions(dict.get()), 64);
  }
  {
    // The parent waits too: 128 buckets are not four times 64.
    const ScopedResizePolicy avoid(DictResizePolicy::kAvoid);
    for (int round = 0; round < 10; ++round) {
      ExpectHolds(dict.get(), 65);
    }
    EXPECT_EQ(ScanPositions(dict.get()), 64);
  }
  // Once the child is gone the rehash finishes.
  for (int round = 0; round < 10; ++round) {
    ExpectHolds(dict.get(), 65);
  }
  EXPECT_EQ(ScanPositions(dict.get()), 128);
}

TEST(DictResizePolicyTest, AvoidShrinksOnlyAVeryEmptyTable) {
  auto dict = Dict<int, int>::Create(256);
  for (int key = 0; key < 200; ++key) {
    ASSERT_TRUE(dict->Insert(key, key));
  }
  const ScopedResizePolicy avoid(DictResizePolicy::kAvoid);
  // 32 of 256 buckets used is an eighth, where a shrink normally starts.
  for (int key = 199; key >= 32; --key) {
    ASSERT_TRUE(dict->Delete(key));
  }
  EXPECT_EQ(ScanPositions(dict.get()), 256);
  // At a thirty-second it shrinks even now.
  for (int key = 31; key >= 8; --key) {
    ASSERT_TRUE(dict->Delete(key));
  }
  for (int round = 0; round < 40; ++round) {
    ExpectHolds(dict.get(), 8);
  }
  EXPECT_EQ(ScanPositions(dict.get()), 8);
}

TEST(DictRehashSomeTest, FinishesARehashWithoutOtherOperations) {
  auto dict = Dict<int, int>::Create(64);
  EXPECT_FALSE(dict->IsRehashing());
  EXPECT_FALSE(dict->RehashSome(100));
  for (int key = 0; key < 65; ++key) {
    ASSERT_TRUE(dict->Insert(key, key));
  }
  // The 65th insert began a rehash from 64 buckets to 128.
  ASSERT_TRUE(dict->IsRehashing());
  EXPECT_TRUE(dict->RehashSome(8));
  EXPECT_TRUE(dict->IsRehashing());
  int steps = 1;
  while (dict->RehashSome(8)) {
    ++steps;
    ASSERT_LE(steps, 64);
  }
  EXPECT_GT(steps, 1);
  EXPECT_FALSE(dict->IsRehashing());
  EXPECT_EQ(ScanPositions(dict.get()), 128);
  ExpectHolds(dict.get(), 65);
}

TEST(DictRehashSomeTest, WaitsForIteratorsAndTheResizePolicy) {
  auto dict = Dict<int, int>::Create(64);
  for (int key = 0; key < 65; ++key) {
    ASSERT_TRUE(dict->Insert(key, key));
  }
  ASSERT_TRUE(dict->IsRehashing());
  {
    // An iterator needs every entry to stay in its table.
    auto it = Dict<int, int>::Iterator(dict.get());
    it.SeekToFirst();
    EXPECT_FALSE(dict->RehashSome(100));
  }
  {
    const ScopedResizePolicy forbidden(DictResizePolicy::kForbidden);
    EXPECT_FALSE(dict->RehashSome(100));
  }
  {
    // 128 buckets are not four times 64, so this rehash waits for the child.
    const ScopedResizePolicy avoid(DictResizePolicy::kAvoid);
    EXPECT_FALSE(dict->RehashSome(100));
  }
  EXPECT_TRUE(dict->IsRehashing());
  EXPECT_EQ(ScanPositions(dict.get()), 64);
  EXPECT_FALSE(dict->RehashSome(100));
  EXPECT_FALSE(dict->IsRehashing());
  EXPECT_EQ(ScanPositions(dict.get()), 128);
  ExpectHolds(dict.get(), 65);
}

TEST(DictRehashSomeTest, ShrinkIfNeededRetriesAShrinkThePolicyHeldBack) {
  auto dict = Dict<int, int>::Create(256);
  for (int key = 0; key < 200; ++key) {
    ASSERT_TRUE(dict->Insert(key, key));
  }
  {
    const ScopedResizePolicy avoid(DictResizePolicy::kAvoid);
    for (int key = 199; key >= 16; --key) {
      ASSERT_TRUE(dict->Delete(key));
    }
    // A sixteenth of the buckets used: sparse, but not enough to shrink
    // while a child runs.
    dict->ShrinkIfNeeded();
    EXPECT_FALSE(dict->IsRehashing());
  }
  // Nothing has been deleted since, so only a retry starts the shrink.
  ExpectHolds(dict.get(), 16);
  EXPECT_EQ(ScanPositions(dict.get()), 256);
  dict->ShrinkIfNeeded();
  EXPECT_TRUE(dict->IsRehashing());
  while (dict->RehashSome(100)) {
  }
  EXPECT_FALSE(dict->IsRehashing());
  EXPECT_EQ(ScanPositions(dict.get()), 16);
  ExpectHolds(dict.get(), 16);
  // A table that is full enough is left alone.
  dict->ShrinkIfNeeded();
  EXPECT_FALSE(dict->IsRehashing());
}
}  // namespace redis_simple::containers
