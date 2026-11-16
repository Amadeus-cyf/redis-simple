#include "server/db/redis_object.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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

TEST(RedisObjectTest, OnlyLargeStringsAreShared) {
  const std::string large(RedisObject::kMinSharedStringBytes, 'a');
  const std::string below(RedisObject::kMinSharedStringBytes - 1, 'b');

  const auto moved = RedisObject::CreateWithString(large);
  const auto copied = RedisObject::CreateWithStringCopy(large);
  for (const RedisObject* object : {moved.get(), copied.get()}) {
    const std::shared_ptr<const std::string> bytes =
        object->SharedStringBytes();
    ASSERT_NE(bytes, nullptr);
    EXPECT_EQ(*bytes, large);
    EXPECT_EQ(object->String(), large);
    EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kRaw);
    EXPECT_EQ(object->Type(), RedisObject::ObjectType::kString);
  }

  EXPECT_EQ(RedisObject::CreateWithString(below)->SharedStringBytes(), nullptr);
  EXPECT_EQ(RedisObject::CreateWithStringCopy(below)->SharedStringBytes(),
            nullptr);
  EXPECT_EQ(RedisObject::CreateWithString("12")->SharedStringBytes(), nullptr);
  EXPECT_EQ(RedisObject::CreateWithString("tiny")->SharedStringBytes(),
            nullptr);

  auto object = RedisObject::CreateWithString("tiny");
  object->AssignString(large);
  EXPECT_NE(object->SharedStringBytes(), nullptr);
  object->ReplaceWithString(below);
  EXPECT_EQ(object->SharedStringBytes(), nullptr);
  object->ReplaceWithString(large);
  EXPECT_NE(object->SharedStringBytes(), nullptr);
  // A raw buffer that an overwrite grows in place becomes shared too.
  object->ReplaceWithString(std::string(10'000, 'c'));
  ASSERT_EQ(object->SharedStringBytes(), nullptr);
  object->AssignString(large);
  EXPECT_NE(object->SharedStringBytes(), nullptr);
  EXPECT_EQ(object->String(), large);
}

TEST(RedisObjectTest, HeldSharedBytesSurviveWritesAndDestruction) {
  const std::string first(RedisObject::kMinSharedStringBytes, 'a');
  const std::string second(RedisObject::kMinSharedStringBytes + 7, 'b');
  auto object = RedisObject::CreateWithString(first);

  const std::shared_ptr<const std::string> held_first =
      object->SharedStringBytes();
  object->AssignString(second);
  EXPECT_EQ(*held_first, first);
  EXPECT_EQ(object->String(), second);

  // An in-place edit, as APPEND makes, works on the object's own copy.
  const std::shared_ptr<const std::string> held_second =
      object->SharedStringBytes();
  ASSERT_NE(held_second, nullptr);
  object->MutableString()->append("tail");
  EXPECT_EQ(*held_second, second);
  EXPECT_EQ(object->String(), second + "tail");
  EXPECT_EQ(object->SharedStringBytes(), nullptr);

  object->ReplaceWithString(first);
  const std::shared_ptr<const std::string> held_third =
      object->SharedStringBytes();
  ASSERT_NE(held_third, nullptr);
  object->AssignString("7");
  EXPECT_EQ(*held_third, first);
  EXPECT_EQ(object->StringEncoding(), RedisObject::Encoding::kInt);

  object->AssignString(second);
  const std::shared_ptr<const std::string> held_last =
      object->SharedStringBytes();
  object.reset();
  EXPECT_EQ(*held_last, second);
  EXPECT_EQ(held_last.use_count(), 1);
}

TEST(RedisObjectTest, SharedBufferNobodyHoldsIsReused) {
  const std::string first(RedisObject::kMinSharedStringBytes, 'a');
  const std::string second(RedisObject::kMinSharedStringBytes, 'b');
  auto object = RedisObject::CreateWithString(first);
  const char* const buffer = object->SharedStringBytes()->data();

  object->AssignString(second);
  ASSERT_NE(object->SharedStringBytes(), nullptr);
  EXPECT_EQ(object->SharedStringBytes()->data(), buffer);
  EXPECT_EQ(object->String(), second);

  // Assigning the object's own bytes, held or not, keeps them.
  object->AssignString(object->String().View().substr(1));
  EXPECT_EQ(object->String(), second.substr(1));
  {
    const std::shared_ptr<const std::string> held = object->SharedStringBytes();
    ASSERT_NE(held, nullptr);
    object->AssignString(std::string_view(*held).substr(1));
    EXPECT_EQ(*held, second.substr(1));
  }
  EXPECT_EQ(object->String(), second.substr(2));

  // An in-place edit takes the buffer instead of copying it.
  object->ReplaceWithString(first);
  const char* const shared_buffer = object->SharedStringBytes()->data();
  EXPECT_EQ(object->MutableString()->data(), shared_buffer);
  EXPECT_EQ(object->String(), first);
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
