#include "memory/dict.h"

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

namespace redis_simple::in_memory {
TEST(DictStrTest, Init) {
  auto dict_str = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(dict_str);
  ASSERT_EQ(dict_str->Size(), 0);
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

TEST(DictStrTest, UsesCustomHashAndEqualityPolicies) {
  auto dict = Dict<std::string, int, CaseInsensitiveHash,
                   CaseInsensitiveEqual>::Create();
  ASSERT_TRUE(dict->Insert("Key", 1));
  EXPECT_FALSE(dict->Insert("KEY", 2));
  ASSERT_NE(dict->FindValue(std::string_view("kEy")), nullptr);
  EXPECT_EQ(*dict->FindValue(std::string_view("kEy")), 1);
}

TEST(DictStrTest, FindOrInsertHashesOnceAndKeepsExistingValues) {
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

TEST(DictStrTest, ShrinksAfterMassDeletes) {
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
  std::optional<size_t> cursor = 0;
  while (cursor.has_value()) {
    cursor = dict->Scan(*cursor, [](const std::string&, const int&) {});
    ++visited_buckets;
  }
  EXPECT_EQ(dict->Size(), 6);
  EXPECT_LE(visited_buckets, 64);
}

TEST(DictStrTest, ScanVisitsEveryKeyWhileTheTableResizes) {
  auto dict = Dict<std::string, int>::Create();
  for (int i = 0; i < 1000; ++i) {
    dict->Insert(std::to_string(i), i);
  }
  std::vector<int> seen(2000, 0);
  std::optional<size_t> cursor = 0;
  int step = 0;
  while (cursor.has_value()) {
    cursor = dict->Scan(*cursor, [&seen](const std::string&, const int& v) {
      ++seen[static_cast<size_t>(v)];
    });
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

TEST(DictStrTest, Insert) {
  auto dict_str = Dict<std::string, std::string>::Create();
  auto status = dict_str->Insert("key", "val");
  ASSERT_TRUE(status);
  ASSERT_EQ(dict_str->Size(), 1);

  // Insert a new key.
  const auto opt1 = dict_str->Get("key");
  ASSERT_TRUE(opt1.has_value());
  ASSERT_EQ(opt1.value_or(""), "val");
  ASSERT_EQ(dict_str->Size(), 1);

  // Update the key.
  dict_str->Set("key", "val_update");
  ASSERT_EQ(dict_str->Size(), 1);

  const auto opt2 = dict_str->Get("key");
  ASSERT_TRUE(opt2.has_value());
  ASSERT_EQ(opt2.value_or(""), "val_update");
  ASSERT_EQ(dict_str->Size(), 1);
}

TEST(DictStrTest, InsertDuplicate) {
  auto dict_str = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(dict_str->Insert("key", "val"));
  ASSERT_FALSE(dict_str->Insert("key", "new_val"));
  ASSERT_EQ(dict_str->Size(), 1);

  const auto result = dict_str->Get("key");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value_or(""), "val");
}

TEST(DictStrTest, FindValueSupportsNonOwningLookup) {
  auto dict_str = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(dict_str->Insert("prefix_key_suffix", "val"));

  const std::string lookup = "xxprefix_key_suffixyy";
  auto* value = dict_str->FindValue(std::string_view(lookup).substr(2, 17));

  ASSERT_NE(value, nullptr);
  ASSERT_EQ(*value, "val");
  ASSERT_EQ(dict_str->FindValue(std::string_view("missing")), nullptr);
}

TEST(DictStrTest, Delete) {
  auto dict_str = Dict<std::string, std::string>::Create();
  ASSERT_TRUE(dict_str->Insert("key", "val"));

  auto status = dict_str->Delete("key");
  ASSERT_TRUE(status);
  ASSERT_EQ(dict_str->Size(), 0);

  const auto result = dict_str->Get("key");
  ASSERT_FALSE(result.has_value());
  ASSERT_EQ(result, std::nullopt);
  ASSERT_EQ(dict_str->Size(), 0);
}

TEST(DictStrTest, ExtractTransfersMoveOnlyValue) {
  auto dict = Dict<std::string, std::unique_ptr<int>>::Create();
  dict->Set(std::string("key"), std::make_unique<int>(42));

  auto value = dict->Extract(std::string_view("key"));

  auto extracted = std::move(value).value_or(nullptr);
  ASSERT_NE(extracted, nullptr);
  EXPECT_EQ(*extracted, 42);
  EXPECT_EQ(dict->Size(), 0);
  EXPECT_FALSE(dict->Extract(std::string_view("missing")).has_value());
}

namespace {
std::unique_ptr<Dict<int, int>> MakeIntDictWithEntries() {
  auto dict_int = Dict<int, int>::Create();
  for (int i = 0; i < 129; ++i) {
    dict_int->Insert(i, i);
  }
  return dict_int;
}
}  // namespace

TEST(DictIntTest, Init) {
  auto dict_int = Dict<int, int>::Create();
  ASSERT_TRUE(dict_int);
  ASSERT_EQ(dict_int->Size(), 0);
}

TEST(DictIntTest, BatchInsert) {
  auto dict_int = MakeIntDictWithEntries();
  ASSERT_EQ(dict_int->Size(), 129);
  const auto result = dict_int->Get(96);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value_or(0), 96);
}

TEST(DictIntTest, Iterator) {
  auto dict_int = MakeIntDictWithEntries();
  auto it = in_memory::Dict<int, int>::Iterator(dict_int.get());
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
  auto dict_int = MakeIntDictWithEntries();
  dict_int->Clear();
  ASSERT_EQ(dict_int->Size(), 0);

  const auto result = dict_int->Get(96);
  ASSERT_FALSE(result.has_value());
  ASSERT_EQ(result, std::nullopt);

  ASSERT_TRUE(dict_int->Insert(1, 10));
  ASSERT_EQ(dict_int->Size(), 1);
  ASSERT_EQ(dict_int->Get(1), 10);
}
}  // namespace redis_simple::in_memory
