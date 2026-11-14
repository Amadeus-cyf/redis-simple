#include "containers/dict.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
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
    const auto next = dict->Scan(cursor, [](const std::string&, const int&) {});
    ++visited_buckets;
    if (!next.has_value()) {
      break;
    }
    cursor = *next;
  }
  EXPECT_EQ(dict->Size(), 6);
  EXPECT_LE(visited_buckets, 64);
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
        dict->Scan(cursor, [&seen](const std::string&, const int& v) {
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
  EXPECT_EQ(dict->ScanStep(0, 10, [](const std::string&, const int&) {}), 0);
  for (int i = 0; i < 1000; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  std::vector<int> seen(1000, 0);
  size_t cursor = 0;
  while (true) {
    size_t visited = 0;
    cursor = dict->ScanStep(
        cursor, 10, [&seen, &visited](const std::string&, const int& v) {
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
}  // namespace redis_simple::containers
