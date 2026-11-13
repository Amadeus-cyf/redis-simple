#include "data_types/list/list.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace redis_simple::data_types {
TEST(ListTest, Push) {
  auto list = List::Create();
  ASSERT_TRUE(list->LPush("12345"));
  ASSERT_TRUE(list->RPush("test string 0"));
  ASSERT_TRUE(list->LPush("hello world"));
  ASSERT_TRUE(list->RPush("test string 1"));
  ASSERT_TRUE(list->LPush("1234567"));
  ASSERT_EQ(list->Size(), 5);
  ASSERT_EQ(list->Encoding(), List::Encoding::kListpack);
  ASSERT_EQ(list->NodeCount(), 0);
}

TEST(ListTest, Pop) {
  auto list = List::Create();
  ASSERT_TRUE(list->LPush("12345"));
  ASSERT_TRUE(list->RPush("test string 0"));
  ASSERT_TRUE(list->LPush("hello world"));
  ASSERT_TRUE(list->RPush("test string 1"));
  ASSERT_TRUE(list->LPush("1234567"));

  ASSERT_EQ(list->LPop(), "1234567");
  ASSERT_EQ(list->RPop(), "test string 1");
  ASSERT_EQ(list->Size(), 3);
  ASSERT_EQ(list->Encoding(), List::Encoding::kListpack);
}

TEST(ListTest, PopEmpty) {
  auto list = List::Create();
  ASSERT_EQ(list->LPop(), std::nullopt);
  ASSERT_EQ(list->RPop(), std::nullopt);
  ASSERT_EQ(list->Size(), 0);
  ASSERT_EQ(list->NodeCount(), 0);
  ASSERT_EQ(list->Encoding(), List::Encoding::kListpack);
}

TEST(ListTest, RangeWithListpackEncoding) {
  auto list = List::Create();
  ASSERT_TRUE(list->RPush("one"));
  ASSERT_TRUE(list->RPush("two"));
  ASSERT_TRUE(list->RPush("three"));
  ASSERT_TRUE(list->RPush("four"));

  ASSERT_EQ(list->Range(0, 3),
            (std::vector<std::string>{"one", "two", "three", "four"}));
  ASSERT_EQ(list->Range(1, 2), (std::vector<std::string>{"two", "three"}));
  ASSERT_EQ(list->Range(2, 100), (std::vector<std::string>{"three", "four"}));
  ASSERT_TRUE(list->Range(4, 5).empty());
  ASSERT_TRUE(list->Range(2, 1).empty());
}

TEST(ListTest, IndexSetRemoveAndTrimWithListpackEncoding) {
  auto list = List::Create();
  ASSERT_TRUE(list->RPush("one"));
  ASSERT_TRUE(list->RPush("two"));
  ASSERT_TRUE(list->RPush("two"));
  ASSERT_TRUE(list->RPush("three"));

  ASSERT_EQ(list->At(0), "one");
  ASSERT_EQ(list->At(3), "three");
  ASSERT_EQ(list->At(4), std::nullopt);

  ASSERT_TRUE(list->Set(1, "changed"));
  ASSERT_FALSE(list->Set(4, "missing"));
  ASSERT_EQ(list->Range(0, 3),
            (std::vector<std::string>{"one", "changed", "two", "three"}));

  ASSERT_EQ(list->Remove("two", 0, List::RemoveDirection::kFromHead), 1);
  ASSERT_EQ(list->Range(0, 2),
            (std::vector<std::string>{"one", "changed", "three"}));

  ASSERT_TRUE(list->Trim(1, 2));
  ASSERT_EQ(list->Range(0, 1), (std::vector<std::string>{"changed", "three"}));
}

TEST(ListTest, RemoveFromTailWithListpackEncoding) {
  auto list = List::Create();
  ASSERT_TRUE(list->RPush("a"));
  ASSERT_TRUE(list->RPush("b"));
  ASSERT_TRUE(list->RPush("c"));
  ASSERT_TRUE(list->RPush("b"));
  ASSERT_TRUE(list->RPush("b"));
  ASSERT_TRUE(list->RPush("d"));

  ASSERT_EQ(list->Remove("b", 2, List::RemoveDirection::kFromTail), 2);

  ASSERT_EQ(list->Encoding(), List::Encoding::kListpack);
  ASSERT_EQ(list->Range(0, 3), (std::vector<std::string>{"a", "b", "c", "d"}));

  ASSERT_EQ(list->Remove("b", 0, List::RemoveDirection::kFromTail), 1);
  ASSERT_EQ(list->Range(0, 2), (std::vector<std::string>{"a", "c", "d"}));
}

TEST(ListTest, SetConvertsListpackToQuicklistWhenValueGrows) {
  auto list = List::Create(48);
  const std::string value(64, 'x');
  ASSERT_TRUE(list->RPush("a"));
  ASSERT_TRUE(list->RPush("b"));
  ASSERT_EQ(list->Encoding(), List::Encoding::kListpack);

  ASSERT_TRUE(list->Set(0, value));

  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);
  ASSERT_EQ(list->Range(0, 1), (std::vector<std::string>{value, "b"}));
}

TEST(ListEncodingTest, ConvertsFromListpackToQuicklistWhenListGrows) {
  auto list = List::Create(96);
  const std::string value(32, 'x');

  ASSERT_TRUE(list->RPush(value + "1"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_EQ(list->Encoding(), List::Encoding::kListpack);

  ASSERT_TRUE(list->RPush(value + "3"));

  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);
  ASSERT_EQ(list->Size(), 3);
  ASSERT_EQ(list->NodeCount(), 2);
  ASSERT_EQ(list->LPop(), value + "1");
  ASSERT_EQ(list->LPop(), value + "2");
  ASSERT_EQ(list->LPop(), value + "3");
}

TEST(ListEncodingTest, ConvertsBackToListpackAfterShrinking) {
  auto list = List::Create(96);
  const std::string value(32, 'y');

  ASSERT_TRUE(list->RPush(value + "1"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_TRUE(list->RPush(value + "3"));
  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);

  ASSERT_EQ(list->RPop(), value + "3");
  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);
  ASSERT_EQ(list->RPop(), value + "2");

  ASSERT_EQ(list->Encoding(), List::Encoding::kListpack);
  ASSERT_EQ(list->Size(), 1);
  ASSERT_EQ(list->NodeCount(), 0);
  ASSERT_EQ(list->LPop(), value + "1");
}

TEST(ListEncodingTest, RangeWithQuicklistEncoding) {
  auto list = List::Create(96);
  const std::string value(32, 'z');

  ASSERT_TRUE(list->RPush(value + "1"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_TRUE(list->RPush(value + "3"));
  ASSERT_TRUE(list->RPush(value + "4"));
  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);

  ASSERT_EQ(list->Range(1, 3),
            (std::vector<std::string>{value + "2", value + "3", value + "4"}));
}

TEST(ListEncodingTest, MutatesQuicklistEncoding) {
  auto list = List::Create(96);
  const std::string value(32, 'q');

  ASSERT_TRUE(list->RPush(value + "1"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_TRUE(list->RPush(value + "3"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);

  ASSERT_EQ(list->At(2), value + "3");
  ASSERT_TRUE(list->Set(2, value + "4"));
  ASSERT_EQ(list->Remove(value + "2", 1, List::RemoveDirection::kFromTail), 1);
  ASSERT_TRUE(list->Trim(1, 2));

  ASSERT_EQ(list->Range(0, 1),
            (std::vector<std::string>{value + "2", value + "4"}));
}

TEST(ListEncodingTest, TrimKeepsQuicklistWhenRangeExceedsListpackLimit) {
  auto list = List::Create(96);
  const std::string value(32, 't');

  ASSERT_TRUE(list->RPush(value + "1"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_TRUE(list->RPush(value + "3"));
  ASSERT_TRUE(list->RPush(value + "4"));
  ASSERT_TRUE(list->RPush(value + "5"));
  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);

  ASSERT_TRUE(list->Trim(0, 3));

  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);
  ASSERT_EQ(list->Range(0, 3),
            (std::vector<std::string>{value + "1", value + "2", value + "3",
                                      value + "4"}));
}

TEST(ListEncodingTest, RemoveFromTailKeepsOriginalOrderWithoutRangeCopy) {
  auto list = List::Create(96);
  const std::string value(32, 'r');

  ASSERT_TRUE(list->RPush(value + "1"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_TRUE(list->RPush(value + "3"));
  ASSERT_TRUE(list->RPush(value + "2"));
  ASSERT_TRUE(list->RPush(value + "4"));
  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);

  ASSERT_EQ(list->Remove(value + "2", 1, List::RemoveDirection::kFromTail), 1);

  ASSERT_EQ(list->Encoding(), List::Encoding::kQuicklist);
  ASSERT_EQ(list->Range(0, 3),
            (std::vector<std::string>{value + "1", value + "2", value + "3",
                                      value + "4"}));
}

TEST(ListEncodingTest, ConvertsBackToListpackAfterTrimRemoveOrSet) {
  const std::string value(32, 'y');
  const auto make_quicklist = [&value] {
    auto list = List::Create(96);
    for (int index = 0; index < 4; ++index) {
      EXPECT_TRUE(list->RPush(value + std::to_string(index)));
    }
    EXPECT_EQ(list->Encoding(), List::Encoding::kQuicklist);
    return list;
  };

  std::unique_ptr<List> trimmed = make_quicklist();
  ASSERT_TRUE(trimmed->Trim(1, 1));
  EXPECT_EQ(trimmed->Encoding(), List::Encoding::kListpack);
  EXPECT_EQ(trimmed->Range(0, 0), std::vector<std::string>{value + "1"});

  std::unique_ptr<List> removed = make_quicklist();
  for (int index = 1; index < 4; ++index) {
    ASSERT_EQ(removed->Remove(value + std::to_string(index), 0,
                              List::RemoveDirection::kFromHead),
              1);
  }
  EXPECT_EQ(removed->Encoding(), List::Encoding::kListpack);
  EXPECT_EQ(removed->Range(0, 0), std::vector<std::string>{value + "0"});

  // Shrinking values with LSET converts once the list fits in half the
  // limit, and not before.
  auto set = List::Create(96);
  const std::string large(60, 'L');
  ASSERT_TRUE(set->RPush(large));
  ASSERT_TRUE(set->RPush(large));
  ASSERT_EQ(set->Encoding(), List::Encoding::kQuicklist);
  ASSERT_TRUE(set->Set(0, "a"));
  EXPECT_EQ(set->Encoding(), List::Encoding::kQuicklist);
  ASSERT_TRUE(set->Set(1, "b"));
  EXPECT_EQ(set->Encoding(), List::Encoding::kListpack);
  EXPECT_EQ(set->Range(0, 1), (std::vector<std::string>{"a", "b"}));

  // A list still above half the limit stays a quicklist.
  std::unique_ptr<List> kept = make_quicklist();
  ASSERT_TRUE(kept->Trim(0, 2));
  EXPECT_EQ(kept->Encoding(), List::Encoding::kQuicklist);
}
}  // namespace redis_simple::data_types
