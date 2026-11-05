#include "data_types/set/set.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::set {
TEST(SetTest, IntegerMembersUseIntSet) {
  auto set = Set::Create();

  ASSERT_TRUE(set->Add("1"));
  ASSERT_TRUE(set->Add("65535"));
  ASSERT_TRUE(set->Add("-65536"));
  ASSERT_TRUE(set->Add("4294967295"));
  ASSERT_TRUE(set->Add("-4294967296"));
  ASSERT_TRUE(set->Add("9223372036854775807"));
  ASSERT_TRUE(set->Add("-9223372036854775808"));

  ASSERT_EQ(set->Encoding(), Set::Encoding::kIntSet);
  ASSERT_EQ(set->Size(), 7);
  ASSERT_TRUE(set->HasMember("-9223372036854775808"));
  ASSERT_TRUE(set->HasMember("4294967295"));
  ASSERT_FALSE(set->Add("1"));

  const std::vector<std::string> members = set->ListAllMembers();
  ASSERT_EQ(members, std::vector<std::string>(
                         {"-9223372036854775808", "-4294967296", "-65536", "1",
                          "65535", "4294967295", "9223372036854775807"}));
}

TEST(SetTest, AddingFirstNonIntegerConvertsIntSetToListPack) {
  auto set = Set::Create();

  ASSERT_TRUE(set->Add("1"));
  ASSERT_TRUE(set->Add("2"));
  ASSERT_TRUE(set->Add("member"));

  ASSERT_EQ(set->Encoding(), Set::Encoding::kListPack);
  ASSERT_EQ(set->Size(), 3);
  ASSERT_TRUE(set->HasMember("1"));
  ASSERT_TRUE(set->HasMember("2"));
  ASSERT_TRUE(set->HasMember("member"));
  ASSERT_FALSE(set->Add("member"));
}

TEST(SetTest, KeepsNonCanonicalIntegerMembersDistinct) {
  auto set = Set::Create();

  EXPECT_TRUE(set->Add("1"));
  EXPECT_TRUE(set->Add("+1"));
  EXPECT_TRUE(set->Add("0"));
  EXPECT_TRUE(set->Add("-0"));
  EXPECT_EQ(set->Encoding(), Set::Encoding::kListPack);
  EXPECT_EQ(set->Size(), 4);
  EXPECT_TRUE(set->HasMember("1"));
  EXPECT_TRUE(set->HasMember("+1"));
  EXPECT_TRUE(set->HasMember("0"));
  EXPECT_TRUE(set->HasMember("-0"));
}

TEST(SetTest, ListPackConversionToDictReportsNewMemberAdded) {
  auto set = Set::Create();

  ASSERT_TRUE(set->Add("member_0"));
  ASSERT_EQ(set->Encoding(), Set::Encoding::kListPack);
  for (size_t i = 1; i < 128; ++i) {
    ASSERT_TRUE(set->Add("member_" + std::to_string(i)));
  }
  ASSERT_EQ(set->Size(), 128);
  ASSERT_EQ(set->Encoding(), Set::Encoding::kListPack);

  ASSERT_TRUE(set->Add("member_128"));

  ASSERT_EQ(set->Encoding(), Set::Encoding::kDict);
  ASSERT_EQ(set->Size(), 129);
  ASSERT_TRUE(set->HasMember("member_0"));
  ASSERT_TRUE(set->HasMember("member_128"));
  ASSERT_FALSE(set->Add("member_128"));
}

TEST(SetTest, LongMemberConvertsListPackToDict) {
  auto set = Set::Create();

  ASSERT_TRUE(set->Add("member"));
  ASSERT_EQ(set->Encoding(), Set::Encoding::kListPack);

  const std::string long_member(65, 'x');
  ASSERT_TRUE(set->Add(long_member));

  ASSERT_EQ(set->Encoding(), Set::Encoding::kDict);
  ASSERT_EQ(set->Size(), 2);
  ASSERT_TRUE(set->HasMember("member"));
  ASSERT_TRUE(set->HasMember(long_member));
  ASSERT_FALSE(set->Add(long_member));
}

TEST(SetTest, OversizedFirstMemberUsesDict) {
  auto set = Set::Create();
  const std::string long_member(65, 'x');

  ASSERT_TRUE(set->Add(long_member));

  EXPECT_EQ(set->Encoding(), Set::Encoding::kDict);
  EXPECT_EQ(set->Size(), 1);
  EXPECT_TRUE(set->HasMember(long_member));
}

TEST(SetTest, RemoveMembersFromEachEncoding) {
  auto intset = Set::Create();
  ASSERT_TRUE(intset->Add("1"));
  ASSERT_TRUE(intset->Remove("1"));
  ASSERT_FALSE(intset->HasMember("1"));
  ASSERT_FALSE(intset->Remove("1"));

  auto listpack = Set::Create();
  ASSERT_TRUE(listpack->Add("member"));
  ASSERT_TRUE(listpack->Remove("member"));
  ASSERT_FALSE(listpack->HasMember("member"));
  ASSERT_FALSE(listpack->Remove("member"));

  auto dict = Set::Create();
  ASSERT_TRUE(dict->Add("member"));
  ASSERT_TRUE(dict->Add(std::string(65, 'x')));
  ASSERT_EQ(dict->Encoding(), Set::Encoding::kDict);
  ASSERT_TRUE(dict->Remove("member"));
  ASSERT_FALSE(dict->HasMember("member"));
  ASSERT_FALSE(dict->Remove("member"));
}

TEST(SetTest, MembershipWithStoredHashesMatchesPlainMembership) {
  // One set per encoding: intset, listpack, and dict.
  auto ints = Set::Create();
  auto small = Set::Create();
  auto large = Set::Create();
  for (int index = 0; index < 300; ++index) {
    ASSERT_TRUE(ints->Add(std::to_string(index)));
    ASSERT_TRUE(large->Add("member:" + std::to_string(index)));
    if (index < 50) {
      ASSERT_TRUE(small->Add("member:" + std::to_string(index * 2)));
    }
  }
  ASSERT_EQ(ints->Encoding(), Set::Encoding::kIntSet);
  ASSERT_EQ(small->Encoding(), Set::Encoding::kListPack);
  ASSERT_EQ(large->Encoding(), Set::Encoding::kDict);

  const std::vector<const Set*> sets = {ints.get(), small.get(), large.get()};
  for (const Set* source : sets) {
    size_t visited = 0;
    ASSERT_TRUE(source->ForEachMemberWithHash(
        [&](std::string_view member, const in_memory::HashedKey* hashed) {
          // Only dict-encoded sets store hashes.
          EXPECT_EQ(hashed != nullptr,
                    source->Encoding() == Set::Encoding::kDict);
          if (hashed != nullptr) {
            EXPECT_EQ(hashed->key, member);
          }
          for (const Set* other : sets) {
            EXPECT_EQ(other->HasMember(member, hashed),
                      other->HasMember(member))
                << member;
          }
          ++visited;
          return true;
        }));
    EXPECT_EQ(visited, source->Size());
  }
  // A visitor returning false stops the dict-encoded walk too.
  size_t calls = 0;
  EXPECT_FALSE(large->ForEachMemberWithHash(
      [&calls](std::string_view, const in_memory::HashedKey*) {
        ++calls;
        return false;
      }));
  EXPECT_EQ(calls, 1);
}
}  // namespace redis_simple::set
