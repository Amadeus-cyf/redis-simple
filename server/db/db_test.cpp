#include "server/db/db.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "utils/time_utils.h"

namespace redis_simple::db {
TEST(RedisDbTest, SetLookupAndDeleteKey) {
  auto redis_db = RedisDb::Create();

  ASSERT_EQ(redis_db->SetKey("key", RedisObject::CreateWithString("value"), 0),
            DbStatus::kOk);
  const auto* object = redis_db->LookupKey("key");
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(object->Type(), RedisObject::ObjectType::kString);
  EXPECT_EQ(object->String(), "value");

  EXPECT_EQ(redis_db->DeleteKey("key"), DbStatus::kOk);
  EXPECT_EQ(redis_db->LookupKey("key"), nullptr);
  EXPECT_EQ(redis_db->DeleteKey("key"), DbStatus::kError);
}

TEST(RedisDbTest, UnlinkRemovesKeyBeforeReclaimingValue) {
  auto redis_db = RedisDb::Create();
  const int64_t future = utils::NowInMilliseconds() + 60'000;
  ASSERT_EQ(redis_db->SetKey("key",
                             RedisObject::CreateWithString(
                                 std::string(size_t{64} * 1024, 'x')),
                             future),
            DbStatus::kOk);

  EXPECT_EQ(redis_db->UnlinkKey("key"), DbStatus::kOk);
  EXPECT_EQ(redis_db->LookupKey("key"), nullptr);
  EXPECT_EQ(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), -2);
  EXPECT_EQ(redis_db->KeyCount(), 0);
  EXPECT_EQ(redis_db->UnlinkKey("key"), DbStatus::kError);
}

TEST(RedisDbTest, ExpiredKeyIsRemovedOnLookup) {
  auto redis_db = RedisDb::Create();
  ASSERT_EQ(
      redis_db->SetKey("expired", RedisObject::CreateWithString("gone"), 1),
      DbStatus::kOk);

  EXPECT_EQ(redis_db->LookupKey("expired"), nullptr);
  EXPECT_EQ(redis_db->DeleteKey("expired"), DbStatus::kError);
}

TEST(RedisDbTest, ReplacingKeyCanClearOrKeepTtl) {
  auto redis_db = RedisDb::Create();
  const int64_t future = utils::NowInMilliseconds() + 60'000;

  ASSERT_EQ(
      redis_db->SetKey("key", RedisObject::CreateWithString("old"), future),
      DbStatus::kOk);
  EXPECT_GT(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), 0);

  ASSERT_EQ(redis_db->SetKey("key", RedisObject::CreateWithString("new"), 0,
                             ToInt(SetKeyFlag::kKeepTtl)),
            DbStatus::kOk);
  EXPECT_GT(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), 0);
  ASSERT_NE(redis_db->LookupKey("key"), nullptr);
  EXPECT_EQ(redis_db->LookupKey("key")->String(), "new");

  ASSERT_EQ(redis_db->SetKey("key", RedisObject::CreateWithString("fresh"), 0),
            DbStatus::kOk);
  EXPECT_EQ(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), -1);
}

TEST(RedisDbTest, ExpireSamplingWorksWhenFewKeysHaveTtl) {
  auto redis_db = RedisDb::Create();
  for (int index = 0; index < 20; ++index) {
    ASSERT_EQ(redis_db->SetKey("persistent-" + std::to_string(index),
                               RedisObject::CreateWithString("value"), 0),
              DbStatus::kOk);
  }
  ASSERT_EQ(
      redis_db->SetKey("expired", RedisObject::CreateWithString("gone"), 1),
      DbStatus::kOk);

  const auto sample = redis_db->ExpireSome(20, 2);

  EXPECT_EQ(sample.sampled, 1);
  EXPECT_EQ(sample.expired, 1);
  EXPECT_EQ(redis_db->KeyCount(), 20);
}

TEST(RedisDbTest, ExpirePersistAndTtl) {
  auto redis_db = RedisDb::Create();
  const int64_t future = utils::NowInMilliseconds() + 60'000;

  EXPECT_EQ(redis_db->TimeToLive("missing", TtlResolution::kMilliseconds), -2);
  ASSERT_EQ(redis_db->SetKey("key", RedisObject::CreateWithString("value"), 0),
            DbStatus::kOk);
  EXPECT_EQ(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), -1);

  EXPECT_EQ(redis_db->ExpireKeyAt("key", future), DbStatus::kOk);
  EXPECT_GT(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), 0);
  EXPECT_GT(redis_db->TimeToLive("key", TtlResolution::kSeconds), 0);
  EXPECT_EQ(redis_db->PersistKey("key"), DbStatus::kOk);
  EXPECT_EQ(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), -1);
  EXPECT_EQ(redis_db->PersistKey("key"), DbStatus::kError);

  EXPECT_EQ(redis_db->ExpireKeyAt("key", 1), DbStatus::kOk);
  EXPECT_EQ(redis_db->LookupKey("key"), nullptr);
}

TEST(RedisDbTest, AppliesConditionalExpirations) {
  auto redis_db = RedisDb::Create();
  const int64_t lower = utils::NowInMilliseconds() + 60'000;
  const int64_t upper = lower + 60'000;
  ASSERT_EQ(redis_db->SetKey("key", RedisObject::CreateWithString("value"), 0),
            DbStatus::kOk);

  EXPECT_EQ(redis_db->ExpireKeyAt("key", lower, ExpireCondition::kGreaterThan),
            DbStatus::kError);
  EXPECT_EQ(redis_db->ExpireKeyAt("key", lower, ExpireCondition::kIfHasExpiry),
            DbStatus::kError);
  EXPECT_EQ(redis_db->ExpireKeyAt("key", lower, ExpireCondition::kLessThan),
            DbStatus::kOk);
  EXPECT_EQ(redis_db->ExpireKeyAt("key", upper, ExpireCondition::kIfNoExpiry),
            DbStatus::kError);
  EXPECT_EQ(redis_db->ExpireKeyAt("key", upper, ExpireCondition::kLessThan),
            DbStatus::kError);
  EXPECT_EQ(redis_db->ExpireKeyAt("key", upper, ExpireCondition::kGreaterThan),
            DbStatus::kOk);
  EXPECT_EQ(redis_db->ExpireKeyAt("key", lower, ExpireCondition::kIfHasExpiry),
            DbStatus::kOk);

  ASSERT_EQ(redis_db->PersistKey("key"), DbStatus::kOk);
  EXPECT_EQ(redis_db->ExpireKeyAt("key", upper, ExpireCondition::kIfNoExpiry),
            DbStatus::kOk);
  EXPECT_EQ(
      redis_db->ExpireKeyAt("missing", upper, ExpireCondition::kIfNoExpiry),
      DbStatus::kError);
}

TEST(RedisDbTest, TtlRoundsToNearestSecond) {
  auto redis_db = RedisDb::Create();
  const int64_t now = utils::NowInMilliseconds();
  ASSERT_EQ(redis_db->SetKey("key", RedisObject::CreateWithString("value"),
                             now + 60'999),
            DbStatus::kOk);

  EXPECT_EQ(redis_db->TimeToLive("key", TtlResolution::kSeconds), 61);
  EXPECT_GT(redis_db->TimeToLive("key", TtlResolution::kMilliseconds), 60'000);
}

TEST(RedisDbTest, RenameAndFlush) {
  auto redis_db = RedisDb::Create();
  const int64_t future = utils::NowInMilliseconds() + 60'000;

  ASSERT_EQ(
      redis_db->SetKey("old", RedisObject::CreateWithString("value"), future),
      DbStatus::kOk);
  ASSERT_EQ(
      redis_db->SetKey("target", RedisObject::CreateWithString("stale"), 0),
      DbStatus::kOk);

  EXPECT_EQ(redis_db->RenameKey("old", "target"), DbStatus::kOk);
  ASSERT_EQ(redis_db->LookupKey("old"), nullptr);
  ASSERT_NE(redis_db->LookupKey("target"), nullptr);
  EXPECT_EQ(redis_db->LookupKey("target")->String(), "value");
  EXPECT_GT(redis_db->TimeToLive("target", TtlResolution::kMilliseconds), 0);
  EXPECT_EQ(redis_db->RenameKey("missing", "new"), DbStatus::kError);

  EXPECT_EQ(redis_db->KeyCount(), 1);
  redis_db->Flush();
  EXPECT_EQ(redis_db->KeyCount(), 0);
  EXPECT_EQ(redis_db->LookupKey("target"), nullptr);
}

TEST(RedisDbTest, RenameDropsTargetTtlAndAllowsSelfRename) {
  auto redis_db = RedisDb::Create();
  const int64_t future = utils::NowInMilliseconds() + 60'000;
  ASSERT_EQ(redis_db->SetKey("source", RedisObject::CreateWithString("v"), 0),
            DbStatus::kOk);
  ASSERT_EQ(
      redis_db->SetKey("target", RedisObject::CreateWithString("old"), future),
      DbStatus::kOk);

  EXPECT_EQ(redis_db->RenameKey("source", "target"), DbStatus::kOk);
  EXPECT_EQ(redis_db->TimeToLive("target", TtlResolution::kMilliseconds), -1);
  EXPECT_EQ(redis_db->LookupKey("target")->String(), "v");

  EXPECT_EQ(redis_db->RenameKey("target", "target"), DbStatus::kOk);
  EXPECT_EQ(redis_db->LookupKey("target")->String(), "v");
  EXPECT_EQ(redis_db->KeyCount(), 1);
}

TEST(RedisDbTest, ScansKeysIncrementallyWithoutExpiredKeys) {
  auto redis_db = RedisDb::Create();
  ASSERT_EQ(redis_db->ScanKeys(0, 1, [](std::string_view) {}), 0);
  ASSERT_EQ(redis_db->SetKey("alpha", RedisObject::CreateWithString("1"), 0),
            DbStatus::kOk);
  ASSERT_EQ(redis_db->SetKey("beta", RedisObject::CreateWithString("2"), 0),
            DbStatus::kOk);
  ASSERT_EQ(redis_db->SetKey("gamma", RedisObject::CreateWithString("3"), 0),
            DbStatus::kOk);
  ASSERT_EQ(
      redis_db->SetKey("expired", RedisObject::CreateWithString("gone"), 1),
      DbStatus::kOk);

  std::vector<std::string> keys;
  size_t cursor = 0;
  while (true) {
    cursor = redis_db->ScanKeys(
        cursor, 1, [&keys](std::string_view key) { keys.emplace_back(key); });
    if (cursor == 0) {
      break;
    }
  }

  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(keys, std::vector<std::string>({"alpha", "beta", "gamma"}));
}
}  // namespace redis_simple::db
