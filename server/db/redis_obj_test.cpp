#include "server/db/redis_obj.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace redis_simple::db {
TEST(RedisObjectTest, StringObjectExposesStringValue) {
  const auto object = RedisObject::CreateWithString("value");

  EXPECT_EQ(object->Type(), RedisObject::ObjectType::kString);
  EXPECT_EQ(object->String(), "value");
  EXPECT_THROW(object->Set(), std::invalid_argument);
  EXPECT_THROW(object->List(), std::invalid_argument);
  EXPECT_THROW(object->ZSet(), std::invalid_argument);
  EXPECT_THROW(object->Hash(), std::invalid_argument);
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

TEST(RedisObjectTest, NonCanonicalIntegersStayRaw) {
  for (const std::string text :
       {"", "007", "+5", "-0", " 5", "5 ", "1.0", "9223372036854775808",
        "123456789012345678901"}) {
    const auto object = RedisObject::CreateWithString(text);
    EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kRaw) << text;
    EXPECT_EQ(object->String(), text);
  }
}

TEST(RedisObjectTest, IntReadsBothEncodings) {
  EXPECT_EQ(RedisObject::CreateWithInt(12)->Int(), 12);
  EXPECT_EQ(RedisObject::CreateWithString("12")->Int(), 12);
  EXPECT_FALSE(RedisObject::CreateWithString("abc")->Int().has_value());
  EXPECT_THROW(RedisObject::CreateWithSet(set::Set::Create())->Int(),
               std::invalid_argument);
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

TEST(RedisObjectTest, StringValueCopiesKeepIntegerBytes) {
  const auto object = RedisObject::CreateWithInt(123);
  StringValue copy = object->String();
  const StringValue second = copy;
  EXPECT_EQ(second.View(), "123");
}

TEST(RedisObjectTest, CollectionObjectsExposeTypedStorage) {
  const auto set_object = RedisObject::CreateWithSet(set::Set::Create());
  EXPECT_EQ(set_object->Type(), RedisObject::ObjectType::kSet);
  ASSERT_NE(set_object->Set(), nullptr);
  EXPECT_THROW(set_object->String(), std::invalid_argument);

  const auto list_object = RedisObject::CreateWithList(list::List::Create());
  EXPECT_EQ(list_object->Type(), RedisObject::ObjectType::kList);
  ASSERT_NE(list_object->List(), nullptr);
  EXPECT_THROW(list_object->Set(), std::invalid_argument);

  const auto zset_object = RedisObject::CreateWithZSet(zset::ZSet::Create());
  EXPECT_EQ(zset_object->Type(), RedisObject::ObjectType::kZSet);
  ASSERT_NE(zset_object->ZSet(), nullptr);
  EXPECT_THROW(zset_object->List(), std::invalid_argument);

  const auto hash_object = RedisObject::CreateWithHash(hash::Hash::Create());
  EXPECT_EQ(hash_object->Type(), RedisObject::ObjectType::kHash);
  ASSERT_NE(hash_object->Hash(), nullptr);
  EXPECT_THROW(hash_object->ZSet(), std::invalid_argument);
}

TEST(RedisObjectTest, RejectsNullCollectionStorage) {
  EXPECT_EQ(RedisObject::CreateWithSet(nullptr), nullptr);
  EXPECT_EQ(RedisObject::CreateWithList(nullptr), nullptr);
  EXPECT_EQ(RedisObject::CreateWithZSet(nullptr), nullptr);
  EXPECT_EQ(RedisObject::CreateWithHash(nullptr), nullptr);
}
}  // namespace redis_simple::db
