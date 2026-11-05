#include "server/commands/command.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace redis_simple::command {
TEST(CommandRegistryTest, FindsCommandsCaseInsensitively) {
  const auto* get = Find("get");
  ASSERT_NE(get, nullptr);
  EXPECT_EQ(get->name, "GET");
  EXPECT_NE(get->callback, nullptr);

  const auto* zrange = Find("ZRANGE");
  ASSERT_NE(zrange, nullptr);
  EXPECT_EQ(zrange->name, "ZRANGE");
  EXPECT_NE(zrange->callback, nullptr);

  const auto* hset = Find("hset");
  ASSERT_NE(hset, nullptr);
  EXPECT_EQ(hset->name, "HSET");
  EXPECT_NE(hset->callback, nullptr);
}

TEST(CommandRegistryTest, FindsExpandedRedisCommandSet) {
  const std::array names = {
      "EXPIRE", "EXPIREAT",  "PEXPIRE",       "PEXPIREAT", "TTL",
      "PTTL",   "PERSIST",   "RENAME",        "DBSIZE",    "FLUSHDB",
      "INCR",   "DECR",      "DECRBY",        "APPEND",    "MGET",
      "MSET",   "STRLEN",    "INCRBY",        "LINDEX",    "LSET",
      "LREM",   "LTRIM",     "SINTER",        "SUNION",    "SDIFF",
      "ZCOUNT", "ZREVRANGE", "ZRANGEBYSCORE", "HMGET",     "HKEYS",
      "HVALS",  "HINCRBY",   "HELLO",         "PING",      "ECHO",
      "QUIT",   "SCAN",      "BGREWRITEAOF",  "INFO"};
  for (const auto* name : names) {
    const auto* command = Find(name);
    ASSERT_NE(command, nullptr) << name;
    EXPECT_NE(command->callback, nullptr);
  }
}

TEST(CommandRegistryTest, ReturnsNullForUnknownCommand) {
  EXPECT_EQ(Find("UNKNOWN"), nullptr);
  EXPECT_EQ(Find(""), nullptr);
  // Prefixes, extensions, and names longer than any command never match.
  EXPECT_EQ(Find("GE"), nullptr);
  EXPECT_EQ(Find("GETS"), nullptr);
  EXPECT_EQ(Find("ZRANGEBYSCOREX"), nullptr);
  EXPECT_EQ(Find(std::string(64, 'A')), nullptr);
  EXPECT_EQ(Find(std::string_view("GET\0", 4)), nullptr);
  // Case folding is ASCII only.
  EXPECT_EQ(Find("G\xc5T"), nullptr);
}

TEST(CommandRegistryTest, CaseFoldingMatchesOnlyTheSameLetter) {
  for (const std::string_view name : {"GET", "ZADD", "BGREWRITEAOF"}) {
    for (size_t position = 0; position < name.size(); ++position) {
      for (int byte = 0; byte < 256; ++byte) {
        std::string probe(name);
        probe[position] = static_cast<char>(byte);
        const char upper = name[position];
        const auto lower = static_cast<char>(upper - 'A' + 'a');
        const bool same_letter =
            probe[position] == upper || probe[position] == lower;
        const auto* command = Find(probe);
        EXPECT_EQ(command != nullptr && command->name == name, same_letter)
            << name << " position " << position << " byte " << byte;
      }
    }
  }
}

TEST(CommandRegistryTest, FindsEveryCaseVariant) {
  for (const std::string_view name :
       {"get", "Get", "gEt", "GET", "zRaNgEbYsCoRe", "bgrewriteaof", "Exec",
        "unwatch"}) {
    const auto* command = Find(name);
    ASSERT_NE(command, nullptr) << name;
    EXPECT_EQ(command->name.size(), name.size()) << name;
  }
}

TEST(CommandRegistryTest, ExposesCommandMetadata) {
  const auto* get = Find("GET");
  ASSERT_NE(get, nullptr);
  EXPECT_TRUE(get->arity.Accepts(1));
  EXPECT_FALSE(get->arity.Accepts(0));
  EXPECT_EQ(get->access, CommandAccess::kReadOnly);
  EXPECT_TRUE(get->keys.HasKeys());
  EXPECT_EQ(get->keys.first, 0);
  EXPECT_EQ(get->keys.last, 0);

  const auto* mset = Find("MSET");
  ASSERT_NE(mset, nullptr);
  EXPECT_EQ(mset->access, CommandAccess::kWrite);
  EXPECT_EQ(mset->keys.first, 0);
  EXPECT_EQ(mset->keys.last, KeySpec::kAllRemaining);
  EXPECT_EQ(mset->keys.step, 2);

  const auto* ping = Find("PING");
  ASSERT_NE(ping, nullptr);
  EXPECT_EQ(ping->access, CommandAccess::kConnection);
  EXPECT_FALSE(ping->keys.HasKeys());

  const auto* rewrite = Find("BGREWRITEAOF");
  ASSERT_NE(rewrite, nullptr);
  EXPECT_EQ(rewrite->access, CommandAccess::kAdmin);
  EXPECT_FALSE(rewrite->keys.HasKeys());

  const auto* info = Find("INFO");
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->access, CommandAccess::kAdmin);
  EXPECT_TRUE(info->arity.Accepts(0));
  EXPECT_TRUE(info->arity.Accepts(1));
}

TEST(CommandRegistryTest, OnlyAuthAndQuitRunBeforeAuth) {
  const auto* auth = Find("auth");
  ASSERT_NE(auth, nullptr);
  EXPECT_TRUE(auth->allowed_before_auth);
  EXPECT_TRUE(auth->arity.Accepts(1));
  EXPECT_TRUE(auth->arity.Accepts(2));
  EXPECT_FALSE(auth->arity.Accepts(3));

  const auto* quit = Find("QUIT");
  ASSERT_NE(quit, nullptr);
  EXPECT_TRUE(quit->allowed_before_auth);

  for (const auto* name : {"PING", "HELLO", "GET", "SET", "INFO"}) {
    const auto* command = Find(name);
    ASSERT_NE(command, nullptr) << name;
    EXPECT_FALSE(command->allowed_before_auth) << name;
  }
}

TEST(CommandRegistryTest, OnlyTransactionControlRunsImmediatelyInMulti) {
  for (const auto* name : {"EXEC", "DISCARD", "MULTI", "WATCH", "QUIT"}) {
    const auto* command = Find(name);
    ASSERT_NE(command, nullptr) << name;
    EXPECT_FALSE(command->queued_in_multi) << name;
  }
  for (const auto* name : {"UNWATCH", "GET", "SET", "PING", "HELLO", "AUTH"}) {
    const auto* command = Find(name);
    ASSERT_NE(command, nullptr) << name;
    EXPECT_TRUE(command->queued_in_multi) << name;
  }
}
}  // namespace redis_simple::command
