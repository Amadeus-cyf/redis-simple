#include "server/db/redis_object.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::db {
TEST(RedisObjectTest, StringObjectExposesStringValue) {
  const auto object = RedisObject::CreateWithString("value");

  EXPECT_EQ(object->Type(), RedisObject::ObjectType::kString);
  EXPECT_EQ(object->String(), "value");
  EXPECT_EQ(object->Set(), nullptr);
  EXPECT_EQ(object->List(), nullptr);
  EXPECT_EQ(object->ZSet(), nullptr);
  EXPECT_EQ(object->Hash(), nullptr);
}

TEST(RedisObjectTest, CanonicalIntegersUseIntEncoding) {
  for (const std::string text :
       {"0", "42", "-7", "9223372036854775807", "-9223372036854775808"}) {
    const auto object = RedisObject::CreateWithString(text);
    EXPECT_EQ(object->Type(), RedisObject::ObjectType::kString) << text;
    EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kInt) << text;
    EXPECT_EQ(object->String(), text);
    EXPECT_EQ(object->String().size(), text.size());
  }
}

TEST(RedisObjectTest, NonCanonicalIntegersAreNotIntEncoded) {
  for (const std::string text :
       {"", "007", "+5", "-0", " 5", "5 ", "1.0", "9223372036854775808",
        "123456789012345678901"}) {
    const auto object = RedisObject::CreateWithString(text);
    EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kEmbedded)
        << text;
    EXPECT_EQ(object->String(), text);
  }
}

TEST(RedisObjectTest, ShortStringsAreEmbeddedAndLongerOnesRaw) {
  // Embedding adds one word to every object, no more.
  EXPECT_LE(sizeof(RedisObject), sizeof(std::string) + 16);
  for (size_t length = 0; length <= EmbeddedString::kCapacity + 2; ++length) {
    std::string text(length, 'x');
    if (!text.empty()) {
      text.front() = 'a';
      text.back() = 'z';
    }
    const redis_simple::db::RedisObject::Encoding expected =
        length <= EmbeddedString::kCapacity ? RedisObject::Encoding::kEmbedded
                                            : RedisObject::Encoding::kRaw;
    const auto created = RedisObject::CreateWithString(text);
    EXPECT_EQ(created->StringEncoding(), expected) << length;
    EXPECT_EQ(created->String(), text) << length;
    const auto copied = RedisObject::CreateWithStringCopy(text);
    EXPECT_EQ(copied->StringEncoding(), expected) << length;
    EXPECT_EQ(copied->String(), text) << length;
    EXPECT_EQ(copied->Type(), RedisObject::ObjectType::kString);
  }
  EXPECT_EQ(RedisObject::CreateWithStringCopy("-12")->StringEncoding(),
            RedisObject::Encoding::kInt);
  EXPECT_EQ(RedisObject::CreateWithString("12x")->Int(), std::nullopt);
}

TEST(RedisObjectTest, MutableStringConvertsEmbeddedEncoding) {
  const auto object = RedisObject::CreateWithString("embedded");
  ASSERT_EQ(object->StringEncoding(), RedisObject::Encoding::kEmbedded);
  object->MutableString()->append(" and edited");
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kRaw);
  EXPECT_EQ(object->String(), "embedded and edited");
}

TEST(RedisObjectTest, AssignStringFromItsOwnRawBytes) {
  // A short value viewing a large raw buffer is embedded, which frees the
  // buffer, so it must be copied out first.
  const auto object =
      RedisObject::CreateWithString(std::string(500, 'r') + "tail");
  ASSERT_EQ(object->StringEncoding(), RedisObject::Encoding::kRaw);
  object->AssignString(object->String().View().substr(500));
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kEmbedded);
  EXPECT_EQ(object->String(), "tail");
}

TEST(RedisObjectTest, AssignStringFromItsOwnEmbeddedBytes) {
  const auto object = RedisObject::CreateWithString("prefix-suffix");
  ASSERT_EQ(object->StringEncoding(), RedisObject::Encoding::kEmbedded);
  // The view points into the object, which re-embedding resets first.
  const std::string_view own = object->String().View().substr(7);
  object->AssignString(own);
  EXPECT_EQ(object->String(), "suffix");
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kEmbedded);
}

TEST(RedisObjectTest, IntReadsBothEncodings) {
  EXPECT_EQ(RedisObject::CreateWithInt(12)->Int(), 12);
  EXPECT_EQ(RedisObject::CreateWithString("12")->Int(), 12);
  EXPECT_FALSE(RedisObject::CreateWithString("abc")->Int().has_value());
}

TEST(RedisObjectTest, MutableStringConvertsIntEncoding) {
  const auto object = RedisObject::CreateWithInt(10);
  object->MutableString()->append("x");
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kRaw);
  EXPECT_EQ(object->String(), "10x");

  object->SetInt(std::numeric_limits<int64_t>::min());
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kInt);
  EXPECT_EQ(object->String(), "-9223372036854775808");
}

TEST(RedisObjectTest, AssignStringChoosesEncodingAndReusesBuffers) {
  auto object = RedisObject::CreateWithString("initial value");
  object->AssignString("42");
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kInt);
  EXPECT_EQ(object->String(), "42");
  object->AssignString("007");
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kEmbedded);
  EXPECT_EQ(object->String(), "007");

  // A raw buffer is reused for a value that fills a fair share of it.
  object->AssignString(std::string(1000, 'a'));
  const char* const buffer = object->MutableString()->data();
  object->AssignString(std::string(600, 'b'));
  EXPECT_EQ(object->MutableString()->data(), buffer);
  EXPECT_EQ(object->String(), std::string(600, 'b'));

  // Shrinking a large value to a short one embeds it, which releases the
  // oversized buffer.
  object->AssignString("tiny");
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kEmbedded);
  EXPECT_EQ(object->String(), "tiny");

  // A small raw buffer is overwritten in place rather than re-embedded.
  object->AssignString(std::string(50, 'c'));
  ASSERT_EQ(object->StringEncoding(), RedisObject::Encoding::kRaw);
  object->AssignString("short");
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kRaw);
  EXPECT_EQ(object->String(), "short");
  EXPECT_LT(object->MutableString()->capacity(), 1000);
}

TEST(RedisObjectTest, StringValueCopiesKeepIntegerBytes) {
  const auto object = RedisObject::CreateWithInt(123);
  StringValue copy = object->String();
  const StringValue second = copy;
  EXPECT_EQ(second.View(), "123");
}

TEST(RedisObjectTest, CollectionObjectsExposeTypedStorage) {
  const auto set_object = RedisObject::CreateWithSet(data_types::Set::Create());
  EXPECT_EQ(set_object->Type(), RedisObject::ObjectType::kSet);
  ASSERT_NE(set_object->Set(), nullptr);

  EXPECT_EQ(set_object->MutableString(), nullptr);

  const auto list_object =
      RedisObject::CreateWithList(data_types::List::Create());
  EXPECT_EQ(list_object->Type(), RedisObject::ObjectType::kList);
  ASSERT_NE(list_object->List(), nullptr);
  EXPECT_EQ(list_object->Set(), nullptr);

  const auto zset_object =
      RedisObject::CreateWithZSet(data_types::ZSet::Create());
  EXPECT_EQ(zset_object->Type(), RedisObject::ObjectType::kZSet);
  ASSERT_NE(zset_object->ZSet(), nullptr);
  EXPECT_EQ(zset_object->List(), nullptr);

  const auto hash_object =
      RedisObject::CreateWithHash(data_types::Hash::Create());
  EXPECT_EQ(hash_object->Type(), RedisObject::ObjectType::kHash);
  ASSERT_NE(hash_object->Hash(), nullptr);
  EXPECT_EQ(hash_object->ZSet(), nullptr);
}

TEST(RedisObjectTest, RejectsNullCollectionStorage) {
  EXPECT_EQ(RedisObject::CreateWithSet(nullptr), nullptr);
  EXPECT_EQ(RedisObject::CreateWithList(nullptr), nullptr);
  EXPECT_EQ(RedisObject::CreateWithZSet(nullptr), nullptr);
  EXPECT_EQ(RedisObject::CreateWithHash(nullptr), nullptr);
}
TEST(RedisObjectDeathTest, StringAccessorsRequireAStringObject) {
  const auto object = RedisObject::CreateWithSet(data_types::Set::Create());
  EXPECT_DEATH(object->String(), "Check failed");
  EXPECT_DEATH(object->Int(), "Check failed");
  EXPECT_DEATH(object->SetInt(1), "Check failed");
  EXPECT_DEATH(object->AssignString("value"), "Check failed");
}

TEST(RedisObjectTest, TypeNamesRoundTrip) {
  for (const RedisObject::ObjectType type :
       {RedisObject::ObjectType::kString, RedisObject::ObjectType::kSet,
        RedisObject::ObjectType::kList, RedisObject::ObjectType::kZSet,
        RedisObject::ObjectType::kHash}) {
    EXPECT_EQ(TypeFromName(TypeName(type)), type);
  }
  EXPECT_EQ(TypeFromName("ZSET"), RedisObject::ObjectType::kZSet);
  EXPECT_EQ(TypeFromName("none"), std::nullopt);
  EXPECT_EQ(TypeFromName("stream"), std::nullopt);
}
}  // namespace redis_simple::db
