#include "utils/strings.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::utils {
TEST(StringsTest, Split) {
  const std::vector<std::string> str0 = Split("string for testing", " ");
  ASSERT_EQ(str0, std::vector<std::string>({"string", "for", "testing"}));

  const std::vector<std::string> str1 =
      Split("string\ntestfor\nnewlinedelimiter\n", "\n");
  ASSERT_EQ(str1, std::vector<std::string>(
                      {"string", "testfor", "newlinedelimiter"}));

  const std::vector<std::string> str2 = Split("string for test", ",");
  ASSERT_EQ(str2, std::vector<std::string>({"string for test"}));

  const std::vector<std::string> str3 = Split("", ",");
  ASSERT_TRUE(str3.empty());

  const std::vector<std::string> str4 = Split("string for testing", "");
  ASSERT_EQ(str4, std::vector<std::string>({"string for testing"}));

  const std::vector<std::string> str5 = Split("", "");
  ASSERT_EQ(str5, std::vector<std::string>({""}));
}

TEST(StringsTest, SplitView) {
  const std::string command = "set key value";
  const std::vector<std::string_view> args = SplitView(command, " ");

  ASSERT_EQ(args.size(), 3);
  EXPECT_EQ(args[0], "set");
  EXPECT_EQ(args[1], "key");
  EXPECT_EQ(args[2], "value");

  const std::vector<std::string_view> no_delimiter = SplitView(command, ",");
  ASSERT_EQ(no_delimiter.size(), 1);
  EXPECT_EQ(no_delimiter[0], command);

  const std::vector<std::string_view> empty_delimiter = SplitView(command, "");
  ASSERT_EQ(empty_delimiter.size(), 1);
  EXPECT_EQ(empty_delimiter[0], command);
}

TEST(StringsTest, ShiftCString) {
  std::array<char, 14> s1 = {'t', 'e', 's', 't', '_', 'b', 'u',
                             'f', 'f', 'e', 'r', '_', '1'};
  ShiftCString(s1.data(), s1.size(), 10);
  ASSERT_EQ(std::memcmp(s1.data(), "r_1", sizeof("r_1")), 0);

  std::array<char, 14> s2 = {'t', 'e', 's', 't', '_', 'b', 'u',
                             'f', 'f', 'e', 'r', '_', '2'};
  ShiftCString(s2.data(), s2.size(), 14);
  ASSERT_EQ(std::memcmp(s2.data(), "", sizeof("")), 0);

  std::array<char, 14> s3 = {'t', 'e', 's', 't', '_', 'b', 'u',
                             'f', 'f', 'e', 'r', '_', '3'};
  ShiftCString(s3.data(), s3.size(), 0);
  ASSERT_EQ(std::memcmp(s3.data(), "test_buffer_3", sizeof("test_buffer_3")),
            0);

  std::array<char, 14> s4 = {'t', 'e', 's', 't', '_', 'b', 'u',
                             'f', 'f', 'e', 'r', '_', '4'};
  ShiftCString(s4.data(), s4.size(), -1);
  ASSERT_EQ(std::memcmp(s4.data(), "", sizeof("")), 0);

  char* s5 = nullptr;
  ShiftCString(s5, 0, 0);
  ASSERT_EQ(s5, nullptr);
}

TEST(StringsTest, ToUppercase) {
  std::string s1("test");
  ToUppercase(s1);
  ASSERT_EQ(s1, "TEST");

  std::string s2("TEST");
  ToUppercase(s2);
  ASSERT_EQ(s2, "TEST");

  std::string s3("test_with_miXTure_LETTers_12343546");
  ToUppercase(s3);
  ASSERT_EQ(s3, "TEST_WITH_MIXTURE_LETTERS_12343546");

  std::string s4;
  ToUppercase(s4);
  ASSERT_EQ(s4, "");

  std::string s5("123456_**&&&||||%%%%");
  ToUppercase(s5);
  ASSERT_EQ(s5, "123456_**&&&||||%%%%");
}

TEST(StringsTest, ConstantTimeEquals) {
  EXPECT_TRUE(ConstantTimeEquals("", ""));
  EXPECT_TRUE(ConstantTimeEquals("secret", "secret"));
  EXPECT_FALSE(ConstantTimeEquals("secret", "Secret"));
  EXPECT_FALSE(ConstantTimeEquals("secret", "secret!"));
  EXPECT_FALSE(ConstantTimeEquals("secret", ""));
  EXPECT_FALSE(ConstantTimeEquals(std::string_view("a\0", 2), "a"));
}

TEST(StringsTest, EqualsIgnoreCase) {
  EXPECT_TRUE(EqualsIgnoreCase("zrange", "ZRANGE"));
  EXPECT_TRUE(EqualsIgnoreCase("KeepTtl", "KEEPTTL"));
  EXPECT_TRUE(EqualsIgnoreCase("+inf", "+INF"));
  EXPECT_FALSE(EqualsIgnoreCase("set", "get"));
  EXPECT_FALSE(EqualsIgnoreCase("expire", "expires"));
}

TEST(StringsTest, FindsLiteralPrefixOfGlob) {
  EXPECT_EQ(GlobLiteralPrefix("user:*"), "user:");
  EXPECT_EQ(GlobLiteralPrefix("*"), "");
  EXPECT_EQ(GlobLiteralPrefix("user:"), std::nullopt);
  EXPECT_EQ(GlobLiteralPrefix(""), std::nullopt);
  EXPECT_EQ(GlobLiteralPrefix("a*b*"), std::nullopt);
  EXPECT_EQ(GlobLiteralPrefix("user:?*"), std::nullopt);
  EXPECT_EQ(GlobLiteralPrefix("user[0-9]*"), std::nullopt);
  EXPECT_EQ(GlobLiteralPrefix(R"(user\**)"), std::nullopt);
  // Wherever a prefix is found, it decides the same matches as the glob.
  for (const std::string_view value : {"", "user", "user:", "user:42", "x"}) {
    EXPECT_EQ(value.starts_with("user:"), MatchesGlob(value, "user:*"));
    EXPECT_TRUE(MatchesGlob(value, "*"));
  }
}

TEST(StringsTest, MatchesRedisStyleGlobs) {
  EXPECT_TRUE(MatchesGlob("", "*"));
  EXPECT_TRUE(MatchesGlob("user:42", "user:*"));
  EXPECT_TRUE(MatchesGlob("key7", "key?"));
  EXPECT_TRUE(MatchesGlob("file5", "file[0-9]"));
  EXPECT_TRUE(MatchesGlob("filex", "file[^0-9]"));
  EXPECT_TRUE(MatchesGlob("literal*", R"(literal\*)"));
  EXPECT_TRUE(MatchesGlob("[abc", "[abc"));
  EXPECT_TRUE(MatchesGlob("value-m", "value-[z-a]"));

  EXPECT_FALSE(MatchesGlob("user:42", "admin:*"));
  EXPECT_FALSE(MatchesGlob("key42", "key?"));
  EXPECT_FALSE(MatchesGlob("filex", "file[0-9]"));
  EXPECT_FALSE(MatchesGlob("file5", "file[^0-9]"));
  EXPECT_FALSE(MatchesGlob("", "?"));
}

TEST(StringsTest, ToUint64MatchesRedisString2ull) {
  uint64_t value = 1;
  EXPECT_TRUE(ToUint64("0", &value));
  EXPECT_EQ(value, 0);
  EXPECT_TRUE(ToUint64("18446744073709551615", &value));
  EXPECT_EQ(value, std::numeric_limits<uint64_t>::max());
  // Text string2ll rejects falls back to strtoull.
  EXPECT_TRUE(ToUint64("+5", &value));
  EXPECT_EQ(value, 5);
  EXPECT_TRUE(ToUint64("007", &value));
  EXPECT_EQ(value, 7);
  EXPECT_TRUE(ToUint64(" 1", &value));
  EXPECT_EQ(value, 1);
  EXPECT_TRUE(ToUint64(std::string_view("12\0junk", 7), &value));
  EXPECT_EQ(value, 12);

  EXPECT_FALSE(ToUint64("", &value));
  EXPECT_FALSE(ToUint64("-1", &value));
  EXPECT_FALSE(ToUint64("18446744073709551616", &value));
  EXPECT_FALSE(ToUint64("1 ", &value));
  EXPECT_FALSE(ToUint64("abc", &value));
}

TEST(StringsTest, ToInt64) {
  std::string s1("100234567");
  int64_t v1 = 0;
  ASSERT_TRUE(ToInt64(s1, &v1));
  ASSERT_EQ(v1, 100234567);

  std::string s2("-100234567");
  int64_t v2 = 0;
  ASSERT_TRUE(ToInt64(s2, &v2));
  ASSERT_EQ(v2, -100234567);

  // Redis' string2ll accepts no '+'.
  std::string s3("+100234567");
  ASSERT_FALSE(ToInt64(s3, nullptr));

  std::string s4("0");
  int64_t v4 = 0;
  ASSERT_TRUE(ToInt64(s4, &v4));
  ASSERT_EQ(v4, 0);

  std::string s5(" -1234545   ");
  ASSERT_FALSE(ToInt64(s5, nullptr));

  std::string s6(" -1234 abcde");
  ASSERT_FALSE(ToInt64(s6, nullptr));

  std::string s7("-12345-123");
  ASSERT_FALSE(ToInt64(s7, nullptr));

  std::string s8("-12345 56778");
  ASSERT_FALSE(ToInt64(s8, nullptr));

  std::string s9("0000000");
  ASSERT_FALSE(ToInt64(s9, nullptr));

  std::string s10("+0");
  ASSERT_FALSE(ToInt64(s10, nullptr));

  std::string s11("-0");
  ASSERT_FALSE(ToInt64(s11, nullptr));

  std::string s12("0123456");
  ASSERT_FALSE(ToInt64(s12, nullptr));

  std::string s13("1002345670000000");
  int64_t v13 = 0;
  ASSERT_TRUE(ToInt64(s13, &v13));
  ASSERT_EQ(v13, 1002345670000000);

  std::string s14("9223372036854775807");
  int64_t v14 = 0;
  ASSERT_TRUE(ToInt64(s14, &v14));
  ASSERT_EQ(v14, INT64_MAX);

  std::string s15("-9223372036854775808");
  int64_t v15 = 0;
  ASSERT_TRUE(ToInt64(s15, &v15));
  ASSERT_EQ(v15, INT64_MIN);

  std::string s16("9223372036854775808");
  ASSERT_FALSE(ToInt64(s16, nullptr));

  std::string s17("-9223372036854775809");
  ASSERT_FALSE(ToInt64(s17, nullptr));

  std::string s18("-922337203685477580812312");
  ASSERT_FALSE(ToInt64(s18, nullptr));

  std::string s19;
  ASSERT_FALSE(ToInt64(s19, nullptr));
}

TEST(StringsTest, Int64ToString) {
  std::array<char, 21> dst1{};
  std::string s1("1234567");
  ASSERT_EQ(Int64ToString(dst1, 1234567), 7);
  ASSERT_TRUE(std::equal(dst1.data(), dst1.data() + 7, s1.c_str()));

  std::array<char, 21> dst2{};
  std::string s2("-1234567");
  ASSERT_EQ(Int64ToString(dst2, -1234567), 8);
  ASSERT_TRUE(std::equal(dst2.data(), dst2.data() + 8, s2.c_str()));

  std::array<char, 21> dst3{};
  std::string s3("9223372036854775807");
  ASSERT_EQ(Int64ToString(dst3, INT64_MAX), 19);
  ASSERT_TRUE(std::equal(dst3.data(), dst3.data() + 19, s3.c_str()));

  std::array<char, 21> dst4{};
  std::string s4("-9223372036854775808");
  ASSERT_EQ(Int64ToString(dst4, INT64_MIN), 20);
  ASSERT_TRUE(std::equal(dst4.data(), dst4.data() + 20, s4.c_str()));

  std::array<char, 21> dst5{};
  std::string s5("0");
  ASSERT_EQ(Int64ToString(dst5, 0), 1);
  ASSERT_TRUE(std::equal(dst5.data(), dst5.data() + 1, s5.c_str()));

  std::array<char, 1> dst6{};
  ASSERT_EQ(Int64ToString(dst6, 0), 0);
  ASSERT_EQ(dst6[0], '\0');

  std::array<char, 7> dst7{};
  ASSERT_EQ(Int64ToString(dst7, 10000000), 0);
  ASSERT_EQ(dst7[0], '\0');

  std::array<char, 7> dst8{};
  ASSERT_EQ(Int64ToString(dst8, -10000000), 0);
  ASSERT_EQ(dst8[0], '-');
  ASSERT_EQ(dst8[1], '\0');
}

TEST(StringsTest, Uint64ToString) {
  std::array<char, 21> dst1{};
  std::string s1("1234567");
  ASSERT_EQ(Uint64ToString(dst1, 1234567), 7);
  ASSERT_TRUE(std::equal(dst1.data(), dst1.data() + 7, s1.c_str()));

  std::array<char, 21> dst2{};
  std::string s2("9223372036854775807");
  ASSERT_EQ(Uint64ToString(dst2, INT64_MAX), 19);
  ASSERT_TRUE(std::equal(dst2.data(), dst2.data() + 19, s2.c_str()));

  std::array<char, 21> dst3{};
  std::string s3("18446744073709551615");
  ASSERT_EQ(Uint64ToString(dst3, UINT64_MAX), 20);
  ASSERT_TRUE(std::equal(dst3.data(), dst3.data() + 20, s3.c_str()));

  std::array<char, 21> dst4{};
  std::string s4("0");
  ASSERT_EQ(Uint64ToString(dst4, 0), 1);
  ASSERT_TRUE(std::equal(dst4.data(), dst4.data() + 1, s4.c_str()));

  std::array<char, 7> dst5{};
  ASSERT_EQ(Uint64ToString(dst5, 10000000), 0);
  ASSERT_EQ(dst5[0], '\0');
}
}  // namespace redis_simple::utils
