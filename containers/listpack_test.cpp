#include "containers/listpack.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/crc64.h"

namespace redis_simple::containers {
namespace {
size_t RequireIndex(std::optional<size_t> index) {
  EXPECT_TRUE(index.has_value());
  return index.value_or(0);
}

Listpack::ListpackEntry StringEntry(std::string_view value) {
  return {value, 0, false};
}

Listpack::ListpackEntry IntEntry(int64_t value) { return {"", value, true}; }

std::string StringAt(const Listpack& listpack, size_t offset) {
  size_t length = 0;
  const unsigned char* data = listpack.Get(offset, &length);
  return {reinterpret_cast<const char*>(data), length};
}

void ExpectEntryAt(const Listpack& listpack, size_t offset,
                   const Listpack::ListpackEntry& entry) {
  if (!entry.is_integer) {
    EXPECT_EQ(StringAt(listpack, offset), entry.string_value);
    return;
  }

  const std::string expected = std::to_string(entry.integer_value);
  EXPECT_EQ(StringAt(listpack, offset), expected);
  EXPECT_EQ(listpack.IntegerAt(offset), entry.integer_value);
}

std::optional<size_t> ExpectEntriesFrom(
    const Listpack& listpack, size_t start_offset,
    const std::vector<Listpack::ListpackEntry>& entries) {
  std::optional<size_t> offset = start_offset;
  for (const auto& entry : entries) {
    if (!offset.has_value()) {
      ADD_FAILURE() << "listpack ended before all entries were checked";
      return offset;
    }
    ExpectEntryAt(listpack, *offset, entry);
    offset = listpack.Next(*offset);
  }
  return offset;
}

size_t Advance(const Listpack& listpack, size_t offset, size_t steps) {
  for (size_t step = 0; step < steps; ++step) {
    const std::optional<size_t> next = listpack.Next(offset);
    if (!next.has_value()) {
      ADD_FAILURE() << "listpack ended before the requested advance";
      return offset;
    }
    offset = *next;
  }
  return offset;
}

std::vector<Listpack::ListpackEntry> MakeBatchAppendEntries(
    std::string_view s0, std::string_view s1, std::string_view s2) {
  return {
      StringEntry(s0),     IntEntry(INT64_MIN), IntEntry(INT64_MAX),
      IntEntry(INT32_MIN), IntEntry(INT32_MAX), IntEntry(INT32_MAX >> 8),
      IntEntry(127),       StringEntry(s1),     StringEntry(s2),
  };
}

std::vector<Listpack::ListpackEntry> MakeBatchPrependEntries(
    std::string_view s0, std::string_view s1, std::string_view s2) {
  return {
      StringEntry(s0),         IntEntry(INT64_MIN + 7),
      IntEntry(INT64_MAX - 9), IntEntry(INT32_MIN + 7),
      IntEntry(INT32_MAX - 9), IntEntry((INT32_MAX >> 8) - 1),
      IntEntry(125),           StringEntry(s1),
      StringEntry(s2),
  };
}

std::vector<Listpack::ListpackEntry> MakeBatchInsertEntries(
    std::string_view s0, std::string_view s1, std::string_view s2) {
  return {
      StringEntry(s0),          IntEntry(INT64_MIN + 21),
      IntEntry(INT64_MAX - 17), IntEntry(INT32_MIN + 21),
      IntEntry(INT32_MAX - 17), IntEntry((INT32_MAX >> 8) - 17),
      IntEntry(INT16_MAX - 17), IntEntry(123),
      StringEntry(s1),          StringEntry(s2),
  };
}

std::unique_ptr<Listpack> MakeAppendedListpack() {
  auto listpack = std::make_unique<Listpack>();
  listpack->Append(-1234);
  std::string s0("test string 0");
  listpack->Append(s0);
  listpack->Append(INT16_MAX >> 3);
  listpack->Append(INT16_MAX);
  listpack->Append(INT32_MAX >> 8);
  listpack->Append(INT32_MAX);
  listpack->Append(INT64_MAX);
  listpack->Append("-1234567890");
  listpack->Append(std::string(4095, 'a'));
  listpack->Append(std::string(10000, 'c'));
  return listpack;
}

std::unique_ptr<Listpack> MakePrependedListpack() {
  auto listpack = MakeAppendedListpack();
  listpack->Prepend(std::string(4094, 'c'));
  listpack->Prepend(INT64_MAX - 4);
  listpack->Prepend(std::string(4096, 'e'));
  listpack->Prepend(INT16_MIN + 3);
  listpack->Prepend(std::string("test string 2"));
  listpack->Prepend(std::string("123456789"));
  return listpack;
}

std::unique_ptr<Listpack> MakeInsertedListpack() {
  auto listpack = MakePrependedListpack();
  std::string s0("test string 2");
  std::string s1(4094, 'f');
  std::string s2(4097, 'g');
  size_t offset = Advance(*listpack, RequireIndex(listpack->First()), 3);
  listpack->Insert(offset, s0);
  offset = Advance(*listpack, offset, 2);
  listpack->Insert(offset, INT64_MAX - 7);
  offset = Advance(*listpack, offset, 2);
  listpack->Insert(offset, INT32_MIN + 7);
  offset = Advance(*listpack, offset, 1);
  listpack->Insert(offset, s1);
  offset = Advance(*listpack, offset, 2);
  listpack->Insert(offset, s2);
  return listpack;
}

std::unique_ptr<Listpack> MakeReplacedListpack() {
  auto listpack = MakeInsertedListpack();
  size_t offset = RequireIndex(listpack->First());
  for (int i = 0; i < 10; ++i) {
    offset = RequireIndex(listpack->Next(offset));
  }
  listpack->Replace(offset, std::string("test string update"));
  for (int i = 0; i < 7; ++i) {
    offset = RequireIndex(listpack->Next(offset));
  }
  listpack->Replace(offset, 17);
  listpack->Replace(RequireIndex(listpack->First()),
                    std::string("test string update 1"));
  listpack->Replace(RequireIndex(listpack->Last()), 217);
  return listpack;
}

std::unique_ptr<Listpack> MakeBatchAppendedListpack() {
  auto listpack = MakeReplacedListpack();
  std::string s0("test string 1");
  std::string s1("hello world");
  std::string s2("-1234567");
  const auto entries = MakeBatchAppendEntries(s0, s1, s2);
  listpack->BatchAppend(entries);
  return listpack;
}

std::unique_ptr<Listpack> MakeBatchPrependedListpack() {
  auto listpack = MakeBatchAppendedListpack();
  std::string s0("test string 3");
  std::string s1(4094, 'h');
  std::string s2(4098, 'i');
  const auto entries = MakeBatchPrependEntries(s0, s1, s2);
  listpack->BatchPrepend(entries);
  return listpack;
}

std::unique_ptr<Listpack> MakeBatchInsertedListpack() {
  auto listpack = MakeBatchPrependedListpack();
  std::string s0("test string 4");
  std::string s1(4093, 'j');
  std::string s2(4099, 'k');
  const auto entries = MakeBatchInsertEntries(s0, s1, s2);
  size_t offset = Advance(*listpack, RequireIndex(listpack->First()), 3);
  listpack->BatchInsert(offset, entries);
  return listpack;
}
}  // namespace

TEST(ListpackTest, Append) {
  auto listpack = std::make_unique<Listpack>();
  ASSERT_FALSE(listpack->First().has_value());
  ASSERT_FALSE(listpack->Last().has_value());

  ASSERT_TRUE(listpack->Append(-1234));
  std::string s0("test string 0");
  ASSERT_TRUE(listpack->Append(s0));
  ASSERT_EQ(listpack->Size(), 2);

  size_t offset = RequireIndex(listpack->First());
  ASSERT_EQ(offset, Listpack::kListpackHeaderSize);
  ASSERT_EQ(listpack->IntegerAt(offset), -1234);
  size_t l0 = 0;
  unsigned char* c0 = listpack->Get(offset, &l0);
  ASSERT_TRUE(std::equal(c0, c0 + 5, "-1234"));

  size_t l1 = 0;
  offset = RequireIndex(listpack->Next(offset));
  unsigned char* c1 = listpack->Get(offset, &l1);
  ASSERT_EQ(l1, s0.size());
  ASSERT_TRUE(std::equal(c1, c1 + l1, s0.c_str()));

  ASSERT_TRUE(listpack->Append(INT16_MAX >> 3));
  ASSERT_TRUE(listpack->Append(INT16_MAX));
  ASSERT_TRUE(listpack->Append(INT32_MAX >> 8));
  ASSERT_TRUE(listpack->Append(INT32_MAX));
  ASSERT_TRUE(listpack->Append(INT64_MAX));
  ASSERT_TRUE(listpack->Append("-1234567890"));

  std::string s2(4095, 'a');
  std::string s3(10000, 'c');
  ASSERT_TRUE(listpack->Append(s2));
  ASSERT_TRUE(listpack->Append(s3));

  // Insert a string which exceeds the listpack size threshold.
  // std::string s4(UINT32_MAX, 'f');
  // ASSERT_FALSE(listpack->Append(s4));

  ASSERT_EQ(listpack->Size(), 10);
  offset = RequireIndex(listpack->Next(offset));
  ASSERT_EQ(listpack->IntegerAt(offset), INT16_MAX >> 3);
  offset = RequireIndex(listpack->Next(offset));
  ASSERT_EQ(listpack->IntegerAt(offset), INT16_MAX);
  offset = RequireIndex(listpack->Next(offset));
  ASSERT_EQ(listpack->IntegerAt(offset), INT32_MAX >> 8);
  offset = RequireIndex(listpack->Next(offset));
  ASSERT_EQ(listpack->IntegerAt(offset), INT32_MAX);
  offset = RequireIndex(listpack->Next(offset));
  ASSERT_EQ(listpack->IntegerAt(offset), INT64_MAX);

  offset = RequireIndex(listpack->Next(offset));
  size_t l2 = 0;
  unsigned char* c2 = listpack->Get(offset, &l2);
  ASSERT_TRUE(std::equal(c2, c2 + l2, "-1234567890"));
  ASSERT_EQ(listpack->IntegerAt(offset), -1234567890);

  offset = RequireIndex(listpack->Next(offset));
  size_t l3 = 0;
  unsigned char* c3 = listpack->Get(offset, &l3);
  ASSERT_EQ(l3, 4095);
  ASSERT_TRUE(std::equal(c3, c3 + l3, s2.c_str()));

  offset = RequireIndex(listpack->Next(offset));
  size_t l4 = 0;
  unsigned char* c4 = listpack->Get(offset, &l4);
  ASSERT_EQ(l4, 10000);
  ASSERT_TRUE(std::equal(c4, c4 + l4, s3.c_str()));

  // Reach the end of the listpack.
  ASSERT_FALSE(listpack->Next(offset).has_value());
}

TEST(ListpackTest, PreservesNonCanonicalIntegerStrings) {
  Listpack listpack;
  ASSERT_TRUE(listpack.Append("+44"));
  ASSERT_TRUE(listpack.Prepend("-0"));
  ASSERT_TRUE(listpack.BatchAppend({StringEntry("+0"), StringEntry("-42")}));

  std::vector<std::string> values;
  ASSERT_TRUE(listpack.ForEach(0, listpack.Size() - 1,
                               [&values](std::string_view value) {
                                 values.emplace_back(value);
                                 return true;
                               }));
  EXPECT_EQ(values, std::vector<std::string>({"-0", "+44", "+0", "-42"}));
}

TEST(ListpackTest, Prepend) {
  auto listpack = MakeAppendedListpack();
  std::string s0(4094, 'c');
  std::string s1(4096, 'e');
  std::string s2("test string 2");
  std::string s3("123456789");
  listpack->Prepend(s0);
  listpack->Prepend(INT64_MAX - 4);
  listpack->Prepend(s1);
  listpack->Prepend(INT16_MIN + 3);
  listpack->Prepend(s2);
  listpack->Prepend(s3);
  ASSERT_EQ(listpack->Size(), 16);

  size_t offset = RequireIndex(listpack->First());
  ASSERT_EQ(offset, Listpack::kListpackHeaderSize);
  size_t l0 = 0;
  unsigned char* c0 = listpack->Get(offset, &l0);
  ASSERT_EQ(l0, s3.size());
  ASSERT_TRUE(std::equal(c0, c0 + l0, "123456789"));
  ASSERT_EQ(listpack->IntegerAt(offset), 123456789);

  offset = RequireIndex(listpack->Next(offset));
  size_t l1 = 0;
  unsigned char* c1 = listpack->Get(offset, &l1);
  ASSERT_EQ(l1, s2.size());
  ASSERT_TRUE(std::equal(c1, c1 + l1, s2.c_str()));

  offset = RequireIndex(listpack->Next(offset));
  size_t l2 = 0;
  unsigned char* c2 = listpack->Get(offset, &l2);
  ASSERT_EQ(l2, 6);
  ASSERT_TRUE(std::equal(c2, c2 + l2, "-32765"));
  ASSERT_EQ(listpack->IntegerAt(offset), INT16_MIN + 3);

  offset = RequireIndex(listpack->Next(offset));
  size_t l3 = 0;
  unsigned char* c3 = listpack->Get(offset, &l3);
  ASSERT_EQ(l3, s1.size());
  ASSERT_TRUE(std::equal(c3, c3 + l3, s1.c_str()));

  offset = RequireIndex(listpack->Next(offset));
  size_t l4 = 0;
  unsigned char* c4 = listpack->Get(offset, &l4);
  ASSERT_EQ(l4, 19);
  ASSERT_TRUE(std::equal(c4, c4 + l4, "9223372036854775803"));
  ASSERT_EQ(listpack->IntegerAt(offset), INT64_MAX - 4);

  offset = RequireIndex(listpack->Next(offset));
  size_t l5 = 0;
  unsigned char* c5 = listpack->Get(offset, &l5);
  ASSERT_EQ(l5, s0.size());
  ASSERT_TRUE(std::equal(c5, c5 + l5, s0.c_str()));
}

TEST(ListpackTest, Insert) {
  auto listpack = MakePrependedListpack();
  std::string s0("test string 2");
  std::string s1(4094, 'f');
  std::string s2(4097, 'g');

  size_t offset = RequireIndex(listpack->First());
  ASSERT_EQ(offset, Listpack::kListpackHeaderSize);
  offset = Advance(*listpack, offset, 3);
  size_t i0 = offset;
  ASSERT_TRUE(listpack->Insert(i0, s0));

  offset = Advance(*listpack, offset, 2);
  size_t i1 = offset;
  ASSERT_TRUE(listpack->Insert(i1, INT64_MAX - 7));

  offset = Advance(*listpack, offset, 2);
  size_t i2 = offset;
  ASSERT_TRUE(listpack->Insert(i2, INT32_MIN + 7));

  offset = Advance(*listpack, offset, 1);
  size_t i3 = offset;
  ASSERT_TRUE(listpack->Insert(i3, s1));

  offset = Advance(*listpack, offset, 2);
  size_t i4 = offset;
  ASSERT_TRUE(listpack->Insert(i4, s2));

  ASSERT_EQ(listpack->Size(), 21);

  size_t l0 = 0;
  unsigned char* c0 = listpack->Get(i0, &l0);
  ASSERT_EQ(l0, s0.size());
  ASSERT_TRUE(std::equal(c0, c0 + l0, s0.c_str()));

  size_t l1 = 0;
  unsigned char* c1 = listpack->Get(i1, &l1);
  ASSERT_EQ(l1, 19);
  ASSERT_TRUE(std::equal(c1, c1 + l1, "9223372036854775800"));
  ASSERT_EQ(listpack->IntegerAt(i1), INT64_MAX - 7);

  size_t l2 = 0;
  unsigned char* c2 = listpack->Get(i2, &l2);
  ASSERT_EQ(l2, 11);
  ASSERT_TRUE(std::equal(c2, c2 + l2, "-2147483641"));
  ASSERT_EQ(listpack->IntegerAt(i2), INT32_MIN + 7);

  size_t l3 = 0;
  unsigned char* c3 = listpack->Get(i3, &l3);
  ASSERT_EQ(l3, 4094);
  ASSERT_TRUE(std::equal(c3, c3 + l3, s1.c_str()));

  size_t l4 = 0;
  unsigned char* c4 = listpack->Get(i4, &l4);
  ASSERT_EQ(l4, 4097);
  ASSERT_TRUE(std::equal(c4, c4 + l4, s2.c_str()));
}

TEST(ListpackTest, Replace) {
  auto listpack = MakeInsertedListpack();
  // Replace string.
  std::string s0("test string update");
  size_t offset = RequireIndex(listpack->First());
  ASSERT_EQ(offset, Listpack::kListpackHeaderSize);
  for (int i = 0; i < 10; ++i) {
    offset = RequireIndex(listpack->Next(offset));
  }
  size_t element_count = listpack->Size();
  // Get next element to check if it's not changed after the replace.
  size_t l0_next = 0;
  size_t next0 = RequireIndex(listpack->Next(offset));
  unsigned char* c0_next = listpack->Get(next0, &l0_next);
  std::string s0_next(reinterpret_cast<const char*>(c0_next), l0_next);
  // Replace and get the element.
  listpack->Replace(offset, s0);
  ASSERT_EQ(listpack->Size(), element_count);
  size_t l1 = 0;
  size_t l1_next = 0;
  unsigned char* c1 = listpack->Get(offset, &l1);
  ASSERT_EQ(l1, s0.size());
  ASSERT_TRUE(std::equal(c1, c1 + l1, s0.c_str()));
  // Get the next element and check if it is not changed.
  size_t next1 = RequireIndex(listpack->Next(offset));
  unsigned char* c1_next = listpack->Get(next1, &l1_next);
  ASSERT_EQ(l1_next, l0_next);
  ASSERT_TRUE(std::equal(c1_next, c1_next + l1_next, s0_next.c_str()));

  // Replace integer.
  for (int i = 0; i < 7; ++i) {
    offset = RequireIndex(listpack->Next(offset));
  }
  // Get next element to check if it's not changed after the replace.
  size_t l2_next = 0;
  size_t next2 = RequireIndex(listpack->Next(offset));
  unsigned char* c2_next = listpack->Get(next2, &l2_next);
  std::string s2_next(reinterpret_cast<const char*>(c2_next), l2_next);
  // Replace and get the element.
  listpack->Replace(offset, 17);
  ASSERT_EQ(listpack->Size(), element_count);
  size_t l3 = 0;
  size_t l3_next = 0;
  unsigned char* c3 = listpack->Get(offset, &l3);
  ASSERT_EQ(l3, 2);
  ASSERT_TRUE(std::equal(c3, c3 + l3, "17"));
  ASSERT_EQ(listpack->IntegerAt(offset), 17);
  // Get the next element and check if it is not changed.
  size_t next3 = RequireIndex(listpack->Next(offset));
  unsigned char* c3_next = listpack->Get(next3, &l3_next);
  ASSERT_EQ(l3_next, l2_next);
  ASSERT_TRUE(std::equal(c3_next, c3_next + l3_next, s2_next.c_str()));

  // Replace the first element.
  offset = RequireIndex(listpack->First());
  std::string s1("test string update 1");
  // Get next element to check if it's not changed after the replace.
  size_t l4_next = 0;
  size_t next4 = RequireIndex(listpack->Next(offset));
  unsigned char* c4_next = listpack->Get(next4, &l4_next);
  std::string s4_next(reinterpret_cast<const char*>(c4_next), l4_next);
  // Replace and get the element.
  listpack->Replace(offset, s1);
  ASSERT_EQ(listpack->Size(), element_count);
  size_t l5 = 0;
  size_t l5_next = 0;
  unsigned char* c5 = listpack->Get(offset, &l5);
  ASSERT_EQ(l5, s1.size());
  ASSERT_TRUE(std::equal(c5, c5 + l5, s1.c_str()));
  // Get the next element and check if it is not changed.
  size_t next5 = RequireIndex(listpack->Next(offset));
  unsigned char* c5_next = listpack->Get(next5, &l5_next);
  ASSERT_EQ(l5_next, l4_next);
  ASSERT_TRUE(std::equal(c5_next, c5_next + l5_next, s4_next.c_str()));

  // Replace the last element.
  offset = RequireIndex(listpack->Last());
  ASSERT_FALSE(listpack->Next(offset).has_value());
  // Replace and get the element.
  listpack->Replace(offset, 217);
  ASSERT_EQ(listpack->Size(), element_count);
  size_t l7 = 0;
  [[maybe_unused]] size_t l7_next = 0;
  unsigned char* c7 = listpack->Get(offset, &l7);
  ASSERT_EQ(l7, 3);
  ASSERT_TRUE(std::equal(c7, c7 + l7, "217"));
  ASSERT_EQ(listpack->IntegerAt(offset), 217);
  // Check if the element is the last element.
  ASSERT_FALSE(listpack->Next(offset).has_value());
}

TEST(ListpackTest, BatchAppend) {
  auto listpack = MakeReplacedListpack();
  std::string s0("test string 1");
  std::string s1("hello world");
  std::string s2("-1234567");
  const auto entries = MakeBatchAppendEntries(s0, s1, s2);
  size_t offset = listpack->TotalBytes() - 1;
  listpack->BatchAppend(entries);
  ASSERT_EQ(listpack->Size(), 30);
  const std::optional<size_t> end_offset =
      ExpectEntriesFrom(*listpack, offset, entries);
  ASSERT_FALSE(end_offset.has_value());

  // Append an empty list.
  ASSERT_FALSE(listpack->BatchAppend({}));
}

TEST(ListpackTest, BatchPrepend) {
  auto listpack = MakeBatchAppendedListpack();
  std::string s0("test string 3");
  std::string s1(4094, 'h');
  std::string s2(4098, 'i');
  const auto entries = MakeBatchPrependEntries(s0, s1, s2);
  ASSERT_TRUE(listpack->BatchPrepend(entries));
  ASSERT_EQ(listpack->Size(), 39);
  size_t offset = RequireIndex(listpack->First());
  ASSERT_EQ(offset, Listpack::kListpackHeaderSize);
  ExpectEntriesFrom(*listpack, offset, entries);

  // Prepend an empty list.
  ASSERT_FALSE(listpack->BatchPrepend({}));
}

TEST(ListpackTest, BatchInsert) {
  auto listpack = MakeBatchPrependedListpack();
  std::string s0("test string 4");
  std::string s1(4093, 'j');
  std::string s2(4099, 'k');
  const auto entries = MakeBatchInsertEntries(s0, s1, s2);
  size_t offset = RequireIndex(listpack->First());
  ASSERT_EQ(offset, Listpack::kListpackHeaderSize);
  offset = Advance(*listpack, offset, 3);
  ASSERT_TRUE(listpack->BatchInsert(offset, entries));
  ASSERT_EQ(listpack->Size(), 49);
  ExpectEntriesFrom(*listpack, offset, entries);

  // Insert an empty list.
  ASSERT_FALSE(listpack->BatchInsert(0, {}));
}

TEST(ListpackDeathTest, InvalidGetOnEmptyListpack) {
  auto listpack = std::make_unique<Listpack>();
  // Out of bound
  EXPECT_DEATH(listpack->Get(0, nullptr), "Check failed");
  EXPECT_DEATH(listpack->Get(listpack->TotalBytes(), nullptr), "Check failed");
  EXPECT_DEATH(listpack->IntegerAt(0), "Check failed");
  EXPECT_DEATH(listpack->IntegerAt(listpack->TotalBytes()), "Check failed");
}

TEST(ListpackTest, Find) {
  auto listpack = MakeBatchInsertedListpack();
  EXPECT_TRUE(listpack->Find("test string 0").has_value());
  EXPECT_TRUE(listpack->Find("-1234567").has_value());
  EXPECT_FALSE(listpack->Find("string not exist").has_value());
  EXPECT_FALSE(listpack->Find("-12345465657").has_value());
}

TEST(ListpackTest, Iterate) {
  auto listpack = MakeBatchInsertedListpack();
  std::optional<size_t> index = listpack->First();
  ASSERT_EQ(index, Listpack::kListpackHeaderSize);

  std::optional<size_t> previous;
  size_t length = 0;
  while (index.has_value()) {
    ++length;
    previous = index;
    index = listpack->Next(*index);
  }
  ASSERT_EQ(previous, listpack->Last());
  ASSERT_EQ(length, listpack->Size());
}

TEST(ListpackTest, IteratorsDecodeEveryEncoding) {
  // One element per encoding: 7-, 13-, 16-, 24-, 32-, and 64-bit integers,
  // and 6-, 12-, and 32-bit strings, including backlens of more than one byte.
  const std::vector<std::string> expected = {
      "7",
      "-4000",
      "30000",
      "-8000000",
      "2000000000",
      "-9223372036854775808",
      "short",
      std::string(200, 'm'),
      std::string(5000, 'l'),
      "",
  };
  Listpack listpack;
  for (const std::string& value : expected) {
    ASSERT_TRUE(listpack.Append(value));
  }
  ASSERT_EQ(listpack.Size(), expected.size());

  // Collect every value through Get() and Next() as the reference.
  std::vector<std::string> by_index;
  for (std::optional<size_t> index = listpack.First(); index.has_value();
       index = listpack.Next(*index)) {
    by_index.push_back(StringAt(listpack, *index));
  }
  ASSERT_EQ(by_index, expected);
  // Canonical integer strings must use the integer encodings above.
  size_t position = 0;
  for (std::optional<size_t> index = listpack.First(); index.has_value();
       index = listpack.Next(*index), ++position) {
    EXPECT_EQ(listpack.IntegerAt(*index).has_value(), position < 6) << position;
  }

  for (size_t start = 0; start < expected.size(); ++start) {
    for (size_t stop = start; stop < expected.size() + 2; ++stop) {
      std::vector<std::string> forward;
      ASSERT_TRUE(listpack.ForEach(start, stop, [&](std::string_view value) {
        forward.emplace_back(value);
        return true;
      }));
      const size_t last = std::min(stop, expected.size() - 1);
      EXPECT_EQ(forward,
                std::vector<std::string>(
                    expected.begin() + static_cast<std::ptrdiff_t>(start),
                    expected.begin() + static_cast<std::ptrdiff_t>(last) + 1))
          << start << ".." << stop;

      std::vector<std::string> reverse;
      ASSERT_TRUE(
          listpack.ForEachReverse(start, stop, [&](std::string_view value) {
            reverse.emplace_back(value);
            return true;
          }));
      std::ranges::reverse(reverse);
      EXPECT_EQ(reverse, forward) << start << ".." << stop;
    }
  }

  std::vector<std::pair<std::string, std::string>> pairs;
  ASSERT_TRUE(listpack.ForEachPair(
      [&](std::string_view first, std::string_view second) {
        pairs.emplace_back(first, second);
        return true;
      }));
  ASSERT_EQ(pairs.size(), expected.size() / 2);
  for (size_t index = 0; index < pairs.size(); ++index) {
    EXPECT_EQ(pairs[index].first, expected[2 * index]);
    EXPECT_EQ(pairs[index].second, expected[(2 * index) + 1]);
  }

  // A visitor returning false stops each iterator after one call.
  size_t calls = 0;
  const auto stop_now = [&calls](auto&&...) {
    ++calls;
    return false;
  };
  EXPECT_FALSE(listpack.ForEach(0, expected.size() - 1, stop_now));
  EXPECT_FALSE(listpack.ForEachReverse(0, expected.size() - 1, stop_now));
  EXPECT_FALSE(listpack.ForEachPair(stop_now));
  EXPECT_EQ(calls, 3);
}

TEST(ListpackTest, OffsetAtFindsEveryElementFromEitherEnd) {
  // Mixed encodings, so stepping back over each backlen is exercised.
  Listpack listpack;
  std::vector<size_t> offsets;
  for (int i = 0; i < 300; ++i) {
    offsets.push_back(listpack.TotalBytes() - 1);
    if (i % 3 == 0) {
      ASSERT_TRUE(listpack.Append(static_cast<int64_t>(i) * 100'000));
    } else {
      ASSERT_TRUE(listpack.Append(std::string(static_cast<size_t>(i), 'v')));
    }
  }
  for (size_t index = 0; index < offsets.size(); ++index) {
    ASSERT_EQ(listpack.OffsetAt(index), offsets[index]);
  }
  EXPECT_EQ(listpack.OffsetAt(offsets.size()), std::nullopt);
  EXPECT_EQ(Listpack().OffsetAt(0), std::nullopt);
}

TEST(ListpackTest, OffsetAt) {
  auto listpack = MakeBatchInsertedListpack();
  std::vector<size_t> indices;

  std::optional<size_t> index = listpack->First();
  while (index.has_value()) {
    indices.push_back(*index);
    index = listpack->Next(*index);
  }

  ASSERT_EQ(indices.size(), listpack->Size());
  for (size_t i = 0; i < indices.size(); ++i) {
    ASSERT_EQ(listpack->OffsetAt(i), indices[i]);
  }
  ASSERT_EQ(listpack->OffsetAt(indices.size()), std::nullopt);
}

TEST(ListpackTest, Delete) {
  auto listpack = MakeBatchInsertedListpack();
  // Delete the head.
  size_t offset = RequireIndex(listpack->First());
  size_t next_offset = RequireIndex(listpack->Next(offset));
  size_t element_count = listpack->Size();
  size_t l0 = 0;
  unsigned char* c0 = listpack->Get(next_offset, &l0);
  listpack->Delete(offset);
  offset = RequireIndex(listpack->First());
  size_t l1 = 0;
  unsigned char* c1 = listpack->Get(offset, &l1);
  ASSERT_EQ(listpack->Size(), element_count - 1);
  ASSERT_EQ(l0, l1);
  ASSERT_TRUE(std::equal(c1, c1 + l1, c0));

  // Delete an element in the mid.
  offset = RequireIndex(listpack->Next(offset));
  const size_t prev_offset = RequireIndex(listpack->Next(offset));
  offset = RequireIndex(listpack->Next(prev_offset));
  next_offset = RequireIndex(listpack->Next(offset));
  size_t l2 = 0;
  unsigned char* c2 = listpack->Get(next_offset, &l2);
  element_count = listpack->Size();
  listpack->Delete(offset);
  ASSERT_EQ(listpack->Size(), element_count - 1);
  offset = RequireIndex(listpack->Next(prev_offset));
  size_t l3 = 0;
  unsigned char* c3 = listpack->Get(offset, &l3);
  ASSERT_EQ(l2, l3);
  ASSERT_TRUE(std::equal(c3, c3 + l3, c2));

  // Delete the tail.
  offset = RequireIndex(listpack->Last());
  ASSERT_FALSE(listpack->Next(offset).has_value());
  size_t l4 = 0;
  unsigned char* c4 = listpack->Get(next_offset, &l4);
  element_count = listpack->Size();
  listpack->Delete(offset);
  ASSERT_EQ(listpack->Size(), element_count - 1);
  size_t l5 = 0;
  unsigned char* c5 = listpack->Get(next_offset, &l5);
  ASSERT_EQ(l4, l5);
  ASSERT_TRUE(std::equal(c5, c5 + l5, c4));
}

TEST(ListpackTest, DeleteRangeUsesOneContiguousMutation) {
  Listpack listpack;
  for (std::string_view value : {"zero", "one", "two", "three", "four"}) {
    ASSERT_TRUE(listpack.Append(value));
  }

  const std::optional<size_t> first_deleted = listpack.OffsetAt(1);
  ASSERT_TRUE(first_deleted.has_value());
  EXPECT_EQ(listpack.DeleteRange(RequireIndex(first_deleted), 3), 3);
  EXPECT_EQ(listpack.Size(), 2);

  std::vector<std::string> values;
  ASSERT_TRUE(listpack.ForEach(0, listpack.Size() - 1,
                               [&values](std::string_view value) {
                                 values.emplace_back(value);
                                 return true;
                               }));
  EXPECT_EQ(values, (std::vector<std::string>{"zero", "four"}));
  const std::optional<size_t> first = listpack.First();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(listpack.DeleteRange(RequireIndex(first), 100), 2);
  EXPECT_EQ(listpack.Size(), 0);
}

TEST(ListpackTest, DeleteMatchingHonorsDirectionAndLimit) {
  Listpack listpack;
  for (std::string_view value : {"x", "keep", "x", "x", "keep", "x"}) {
    ASSERT_TRUE(listpack.Append(value));
  }

  EXPECT_EQ(listpack.DeleteMatching("x", 2, true), 2);
  EXPECT_EQ(listpack.Size(), 4);
  std::vector<std::string> values;
  ASSERT_TRUE(listpack.ForEach(0, listpack.Size() - 1,
                               [&values](std::string_view value) {
                                 values.emplace_back(value);
                                 return true;
                               }));
  EXPECT_EQ(values, (std::vector<std::string>{"x", "keep", "x", "keep"}));

  EXPECT_EQ(listpack.DeleteMatching("x", 0), 2);
  EXPECT_EQ(listpack.Size(), 2);
}
TEST(ListpackTest, DeleteMatchingMatchesModelForEveryEncoding) {
  // Integers and strings of every length class, so matches and kept runs
  // cover each encoding and backlen width.
  const std::vector<std::string> values = {"x",
                                           "",
                                           "7",
                                           "-129",
                                           "12345678901",
                                           std::string(70, 'm'),
                                           std::string(5000, 'l')};
  // A fixed linear congruential sequence keeps the cases reproducible.
  uint32_t state = 7;
  const auto random = [&state] {
    state = (state * 1664525U) + 1013904223U;
    return state >> 8;
  };
  for (int round = 0; round < 300; ++round) {
    Listpack listpack;
    std::vector<std::string> model;
    const size_t size = random() % 40;
    for (size_t i = 0; i < size; ++i) {
      model.push_back(values[random() % values.size()]);
      ASSERT_TRUE(listpack.Append(model.back()));
    }
    const std::string& target = values[random() % values.size()];
    const size_t limit = random() % 4;
    const bool from_tail = random() % 2 == 0;

    size_t expected = 0;
    if (from_tail && limit != 0) {
      for (size_t i = model.size(); i > 0 && expected < limit; --i) {
        if (model[i - 1] == target) {
          model.erase(model.begin() + static_cast<std::ptrdiff_t>(i - 1));
          ++expected;
        }
      }
    } else {
      for (size_t i = 0;
           i < model.size() && (limit == 0 || expected < limit);) {
        if (model[i] == target) {
          model.erase(model.begin() + static_cast<std::ptrdiff_t>(i));
          ++expected;
        } else {
          ++i;
        }
      }
    }
    ASSERT_EQ(listpack.DeleteMatching(target, limit, from_tail), expected);
    ASSERT_EQ(listpack.Size(), model.size());

    // Walk both ways so every moved element's encoding and backlen is used.
    std::vector<std::string> forward;
    for (std::optional<size_t> index = listpack.First(); index.has_value();
         index = listpack.Next(*index)) {
      forward.push_back(listpack.Get(*index).value_or("<missing>"));
    }
    EXPECT_EQ(forward, model) << round;
    std::vector<std::string> backward;
    for (std::optional<size_t> index = listpack.Last(); index.has_value();
         index = listpack.Prev(*index)) {
      backward.push_back(listpack.Get(*index).value_or("<missing>"));
    }
    std::ranges::reverse(backward);
    EXPECT_EQ(backward, model) << round;
    // The listpack stays appendable after compaction.
    ASSERT_TRUE(listpack.Append("tail"));
    EXPECT_EQ(listpack.Get(RequireIndex(listpack.Last())), "tail");
  }
}

TEST(ListpackTest, DeleteMatchingKeepsSaturatedCount) {
  Listpack listpack;
  constexpr size_t kSize = 70'000;
  for (size_t i = 0; i < kSize; ++i) {
    ASSERT_TRUE(listpack.Append(i % 10 == 0 ? "x" : "y"));
  }
  EXPECT_EQ(listpack.DeleteMatching("x", 0), kSize / 10);
  EXPECT_EQ(listpack.Size(), kSize - (kSize / 10));
  EXPECT_EQ(listpack.DeleteMatching("y", 3, true), 3);
  EXPECT_EQ(listpack.Size(), kSize - (kSize / 10) - 3);
}

TEST(ListpackDeathTest, RejectsOutOfBoundsIndexes) {
  auto listpack = std::make_unique<Listpack>();
  ASSERT_TRUE(listpack->Append("value"));
  ASSERT_TRUE(listpack->Append(42));
  const size_t end = listpack->TotalBytes();
  const auto entries = MakeBatchInsertEntries("a", "b", "c");

  EXPECT_DEATH(listpack->Get(Listpack::kListpackHeaderSize - 1, nullptr),
               "Check failed");
  EXPECT_DEATH(listpack->Get(end, nullptr), "Check failed");
  EXPECT_DEATH(listpack->IntegerAt(3), "Check failed");
  EXPECT_DEATH(listpack->IntegerAt(end), "Check failed");
  EXPECT_DEATH(listpack->Insert(0, "value"), "Check failed");
  EXPECT_DEATH(listpack->Insert(end, 123456), "Check failed");
  EXPECT_DEATH(listpack->Replace(0, "value"), "Check failed");
  EXPECT_DEATH(listpack->Replace(end, 123456789), "Check failed");
  EXPECT_DEATH(listpack->BatchInsert(0, entries), "Check failed");
  EXPECT_DEATH(listpack->BatchInsert(end, entries), "Check failed");
  EXPECT_DEATH(listpack->Delete(Listpack::kListpackHeaderSize - 1),
               "Check failed");
  EXPECT_DEATH(listpack->Delete(end), "Check failed");
}

// Bytes Redis 8.10 produces for the same elements (from DUMP with
// rdbcompression off), so RDB files can store listpacks as they are.
std::string Hex(std::string_view bytes) {
  std::string hex;
  for (const char c : bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    const auto byte = static_cast<unsigned char>(c);
    hex.push_back(kDigits[byte >> 4]);
    hex.push_back(kDigits[byte & 0xf]);
  }
  return hex;
}

TEST(ListpackTest, EncodesIntegersLikeRedis) {
  Listpack listpack;
  for (const std::string_view value : {"0",
                                       "1",
                                       "127",
                                       "128",
                                       "-1",
                                       "4095",
                                       "-4096",
                                       "4096",
                                       "-4097",
                                       "32767",
                                       "-32768",
                                       "32768",
                                       "8388607",
                                       "-8388608",
                                       "8388608",
                                       "2147483647",
                                       "-2147483648",
                                       "2147483648",
                                       "-2147483649",
                                       "1099511627776",
                                       "-1099511627776",
                                       "9223372036854775807",
                                       "-9223372036854775808"}) {
    listpack.Append(value);
  }
  EXPECT_EQ(
      Hex(listpack.Bytes()),
      "860000001700000101017f01c08002dfff02cfff02d00002f1001003f1ffef03f1ff7f"
      "03f1008003f200800004f2ffff7f04f200008004f30000800005f3ffffff7f05f30000"
      "008005f4000000800000000009f4ffffff7fffffffff09f4000000000001000009f400"
      "00000000ffffff09f4ffffffffffffff7f09f4000000000000008009ff");
}

TEST(ListpackTest, EncodesStringsAndBacklensLikeRedis) {
  Listpack small;
  for (const std::string_view value : {"f1", "v1", "n", "12345", "neg", "-7"}) {
    small.Append(value);
  }
  EXPECT_EQ(Hex(small.Bytes()),
            "1e00000006008266310382763103816e02f1393003836e656704dff902ff");

  // Lengths at each string encoding's limits, and backlens of one and two
  // bytes; the 5000-byte string uses the 32-bit length encoding.
  Listpack strings;
  for (const auto& [length, fill] :
       std::initializer_list<std::pair<size_t, char>>{{0, ' '},
                                                      {1, 'a'},
                                                      {63, 'x'},
                                                      {64, 'y'},
                                                      {126, 'z'},
                                                      {127, 'w'},
                                                      {200, 'v'},
                                                      {4095, 'u'}}) {
    strings.Append(std::string(length, fill));
  }
  EXPECT_EQ(strings.Bytes().size(), 4708);
  EXPECT_EQ(utils::Crc64(0, strings.Bytes()), 0xdf2002b80ea90a83ULL);
  Listpack large;
  large.Append(std::string(5000, 'q'));
  EXPECT_EQ(large.Bytes().size(), 5014);
  EXPECT_EQ(utils::Crc64(0, large.Bytes()), 0xa1e049c1b7ab3714ULL);
}
}  // namespace redis_simple::containers
