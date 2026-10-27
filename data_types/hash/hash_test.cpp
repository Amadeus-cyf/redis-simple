#include "data_types/hash/hash.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::hash {
namespace {
bool EntryLess(const Hash::Entry& lhs, const Hash::Entry& rhs) {
  return lhs.field < rhs.field;
}
}  // namespace

TEST(HashTest, SetGetDeleteWithListPackEncoding) {
  auto hash = Hash::Create();

  EXPECT_TRUE(hash->Set("field1", "value1"));
  EXPECT_TRUE(hash->Set("field2", "value2"));
  EXPECT_FALSE(hash->Set("field1", "updated"));

  EXPECT_EQ(hash->Encoding(), Hash::Encoding::kListPack);
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
  std::sort(entries.begin(), entries.end(), EntryLess);

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

TEST(HashTest, VisitValueReturnsListPackValueWithoutCopying) {
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
      std::find_if(entries.begin(), entries.end(),
                   [](const auto& entry) { return entry.field == "field"; });
  const auto long_field_entry = std::find_if(
      entries.begin(), entries.end(),
      [&](const auto& entry) { return entry.field == long_field; });

  ASSERT_EQ(entries.size(), 2);
  ASSERT_NE(field_entry, entries.end());
  ASSERT_NE(long_field_entry, entries.end());
  EXPECT_EQ(field_entry->value, long_value);
  EXPECT_EQ(long_field_entry->value, "value");
}

TEST(HashEncodingTest, ConvertsToDictWhenEntryCountExceedsLimit) {
  auto hash = Hash::Create();

  for (int i = 0; i < 129; ++i) {
    EXPECT_TRUE(
        hash->Set("field" + std::to_string(i), "value" + std::to_string(i)));
  }

  EXPECT_EQ(hash->Encoding(), Hash::Encoding::kDict);
  EXPECT_EQ(hash->Size(), 129);
  EXPECT_EQ(hash->Get("field0"), "value0");
  EXPECT_EQ(hash->Get("field128"), "value128");
}

TEST(HashEncodingTest, VisitValueReturnsDictValueWithoutCopying) {
  auto hash = Hash::Create();
  std::string value;

  for (int i = 0; i < 129; ++i) {
    EXPECT_TRUE(
        hash->Set("field" + std::to_string(i), "value" + std::to_string(i)));
  }

  ASSERT_EQ(hash->Encoding(), Hash::Encoding::kDict);
  EXPECT_TRUE(hash->VisitValue("field128", [&value](std::string_view visited) {
    value.assign(visited);
  }));
  EXPECT_EQ(value, "value128");
}
}  // namespace redis_simple::hash
