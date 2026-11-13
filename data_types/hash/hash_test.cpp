#include "data_types/hash/hash.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::hash {
namespace {
bool EntryLess(const Hash::Entry& lhs, const Hash::Entry& rhs) {
  return lhs.field < rhs.field;
}
}  // namespace

TEST(HashTest, SetGetDeleteWithListpackEncoding) {
  auto hash = Hash::Create();

  EXPECT_TRUE(hash->Set("field1", "value1"));
  EXPECT_TRUE(hash->Set("field2", "value2"));
  EXPECT_FALSE(hash->Set("field1", "updated"));

  EXPECT_EQ(hash->Encoding(), Hash::Encoding::kListpack);
  EXPECT_EQ(hash->Size(), 2);
  EXPECT_TRUE(hash->Exists("field1"));
  EXPECT_FALSE(hash->Exists("missing"));
  EXPECT_EQ(hash->Get("field1"), "updated");
  EXPECT_EQ(hash->Get("field2"), "value2");
  EXPECT_FALSE(hash->Get("missing").has_value());

  EXPECT_TRUE(hash->Delete("field1"));
  EXPECT_FALSE(hash->Delete("field1"));
  EXPECT_EQ(hash->Size(), 1);
  EXPECT_FALSE(hash->Exists("field1"));
  EXPECT_EQ(hash->Get("field2"), "value2");
}

TEST(HashTest, EntriesIncludesAllPairs) {
  auto hash = Hash::Create();

  EXPECT_TRUE(hash->Set("beta", "2"));
  EXPECT_TRUE(hash->Set("alpha", "1"));
  EXPECT_FALSE(hash->Set("beta", "updated"));

  std::vector<Hash::Entry> entries = hash->Entries();
  std::ranges::sort(entries, EntryLess);

  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].field, "alpha");
  EXPECT_EQ(entries[0].value, "1");
  EXPECT_EQ(entries[1].field, "beta");
  EXPECT_EQ(entries[1].value, "updated");
}

TEST(HashTest, FieldLookupDoesNotMatchValues) {
  auto hash = Hash::Create();

  EXPECT_TRUE(hash->Set("field", "same"));
  EXPECT_TRUE(hash->Set("same", "value"));

  EXPECT_EQ(hash->Get("same"), "value");
  EXPECT_TRUE(hash->Delete("same"));
  EXPECT_EQ(hash->Get("field"), "same");
}

TEST(HashTest, VisitValueReturnsListpackValueWithoutCopying) {
  auto hash = Hash::Create();
  std::string value;

  EXPECT_TRUE(hash->Set("field", "42"));
  EXPECT_TRUE(hash->VisitValue(
      "field", [&value](std::string_view visited) { value.assign(visited); }));
  EXPECT_FALSE(hash->VisitValue("missing", [](std::string_view) {}));
  EXPECT_EQ(value, "42");
}

TEST(HashEncodingTest, ConvertsToDictWhenFieldOrValueIsLong) {
  auto hash = Hash::Create();
  const std::string long_value(65, 'v');

  EXPECT_TRUE(hash->Set("field", "value"));
  EXPECT_FALSE(hash->Set("field", long_value));

  EXPECT_EQ(hash->Encoding(), Hash::Encoding::kDict);
  EXPECT_EQ(hash->Size(), 1);
  EXPECT_EQ(hash->Get("field"), long_value);

  const std::string long_field(65, 'f');
  EXPECT_TRUE(hash->Set(long_field, "value"));
  EXPECT_EQ(hash->Size(), 2);
  EXPECT_EQ(hash->Get(long_field), "value");

  const std::vector<Hash::Entry> entries = hash->Entries();
  const auto field_entry =
      std::ranges::find(entries, "field", &Hash::Entry::field);
  const auto long_field_entry =
      std::ranges::find(entries, long_field, &Hash::Entry::field);

  ASSERT_EQ(entries.size(), 2);
  ASSERT_NE(field_entry, entries.end());
  ASSERT_NE(long_field_entry, entries.end());
  EXPECT_EQ(field_entry->value, long_value);
  EXPECT_EQ(long_field_entry->value, "value");
}

TEST(HashEncodingTest, ConvertsToDictWhenEntryCountExceedsLimit) {
  auto hash = Hash::Create();

  // Like Redis, a hash stays a listpack up to 512 entries.
  for (int i = 0; i < 512; ++i) {
    EXPECT_TRUE(
        hash->Set("field" + std::to_string(i), "value" + std::to_string(i)));
  }
  EXPECT_EQ(hash->Encoding(), Hash::Encoding::kListpack);
  EXPECT_TRUE(hash->Set("field512", "value512"));

  EXPECT_EQ(hash->Encoding(), Hash::Encoding::kDict);
  EXPECT_EQ(hash->Size(), 513);
  EXPECT_EQ(hash->Get("field0"), "value0");
  EXPECT_EQ(hash->Get("field512"), "value512");
}

TEST(HashEncodingTest, VisitValueReturnsDictValueWithoutCopying) {
  auto hash = Hash::Create();
  std::string value;

  for (int i = 0; i < 513; ++i) {
    EXPECT_TRUE(
        hash->Set("field" + std::to_string(i), "value" + std::to_string(i)));
  }

  ASSERT_EQ(hash->Encoding(), Hash::Encoding::kDict);
  EXPECT_TRUE(hash->VisitValue("field128", [&value](std::string_view visited) {
    value.assign(visited);
  }));
  EXPECT_EQ(value, "value128");
}

TEST(HashEncodingTest, ScanDictVisitsEveryEntryOnce) {
  auto hash = Hash::Create();
  for (int i = 0; i < 600; ++i) {
    hash->Set("field" + std::to_string(i), "value" + std::to_string(i));
  }
  ASSERT_EQ(hash->Encoding(), Hash::Encoding::kDict);
  std::map<std::string, std::string> seen;
  size_t cursor = 0;
  while (true) {
    cursor = hash->ScanDict(
        cursor, 7, [&seen](std::string_view field, std::string_view value) {
          EXPECT_TRUE(seen.emplace(field, value).second) << field;
        });
    if (cursor == 0) {
      break;
    }
  }
  ASSERT_EQ(seen.size(), 600);
  EXPECT_EQ(seen["field42"], "value42");
}
}  // namespace redis_simple::hash
