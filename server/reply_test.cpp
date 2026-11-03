#include "server/reply.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::reply {
TEST(ReplyTest, FromSimpleString) {
  ASSERT_EQ(FromString("OK"), "+OK\r\n");
  ASSERT_EQ(FromString(""), "+\r\n");
}

TEST(ReplyTest, FromBulkString) {
  ASSERT_EQ(FromBulkString("test bulk string"), "$16\r\ntest bulk string\r\n");
  ASSERT_EQ(FromBulkString(""), "$0\r\n\r\n");
}

TEST(ReplyTest, FromBulkStringArray) {
  ASSERT_EQ(FromBulkStringArray({"one", "two"}),
            "*2\r\n$3\r\none\r\n$3\r\ntwo\r\n");
  ASSERT_EQ(FromBulkStringArray({}), "*0\r\n");
}

TEST(ReplyTest, AppendsBulkStringsAfterExistingContent) {
  std::string encoded = "*3\r\n";
  AppendBulkString("", &encoded);
  const std::string binary("a\0\r\nb", 5);
  AppendBulkString(binary, &encoded);
  const std::string large(100000, 'x');
  AppendBulkString(large, &encoded);
  std::string expected = "*3\r\n$0\r\n\r\n$5\r\n";
  expected.append(binary)
      .append("\r\n$100000\r\n")
      .append(large)
      .append("\r\n");
  EXPECT_EQ(encoded, expected);

  // Many small appends reallocate geometrically, not once per element.
  std::string many;
  size_t reallocations = 0;
  for (int index = 0; index < 10000; ++index) {
    const size_t capacity = many.capacity();
    AppendBulkString(std::to_string(index), &many);
    reallocations += many.capacity() != capacity ? 1 : 0;
  }
  EXPECT_LT(reallocations, 32);
  EXPECT_TRUE(many.starts_with("$1\r\n0\r\n$1\r\n1\r\n"));
  EXPECT_TRUE(many.ends_with("$4\r\n9999\r\n"));
}

TEST(ReplyTest, AppendsFloatsForEachProtocol) {
  std::string encoded = "*2\r\n";
  AppendFloat(2.5, ProtocolVersion::kResp2, &encoded);
  AppendFloat(-3, ProtocolVersion::kResp2, &encoded);
  EXPECT_EQ(encoded, "*2\r\n$3\r\n2.5\r\n$2\r\n-3\r\n");
  encoded.clear();
  AppendFloat(2.5, ProtocolVersion::kResp3, &encoded);
  AppendFloat(1e17, ProtocolVersion::kResp3, &encoded);
  EXPECT_EQ(encoded, ",2.5\r\n,1e+17\r\n");
  EXPECT_EQ(FromFloat(7, ProtocolVersion::kResp2), "$1\r\n7\r\n");
}

TEST(ReplyTest, AppendsArrayHeader) {
  std::string encoded = "+OK\r\n";
  AppendArrayHeader(3, &encoded);
  EXPECT_EQ(encoded, "+OK\r\n*3\r\n");
}

TEST(ReplyTest, From64BitsInt) {
  ASSERT_EQ(FromInt64(1234567), ":1234567\r\n");
}

TEST(ReplyTest, EncodesVersionSpecificTypes) {
  EXPECT_EQ(Null(ProtocolVersion::kResp2), "$-1\r\n");
  EXPECT_EQ(Null(ProtocolVersion::kResp3), "_\r\n");
  EXPECT_EQ(FromFloat(1.5, ProtocolVersion::kResp2), "$3\r\n1.5\r\n");
  EXPECT_EQ(FromFloat(1.5, ProtocolVersion::kResp3), ",1.5\r\n");
  EXPECT_EQ(FromMapHeader(2, ProtocolVersion::kResp2), "*4\r\n");
  EXPECT_EQ(FromMapHeader(2, ProtocolVersion::kResp3), "%2\r\n");
  EXPECT_EQ(FromSetHeader(2, ProtocolVersion::kResp2), "*2\r\n");
  EXPECT_EQ(FromSetHeader(2, ProtocolVersion::kResp3), "~2\r\n");
}

TEST(ReplyTest, FromArray) {
  ASSERT_EQ(
      FromArray({":123\r\n", "+hello world\r\n", "$13\r\nhello world\r\n"}),
      "*3\r\n:123\r\n+hello world\r\n$13\r\nhello world\r\n");
}

TEST(ReplyTest, FromError) {
  ASSERT_EQ(FromError("ERR syntax error"), "-ERR syntax error\r\n");
  ASSERT_EQ(WrongNumberOfArguments("GET"),
            "-ERR wrong number of arguments for 'get' command\r\n");
  ASSERT_EQ(UnknownCommand("NOTACOMMAND", {}),
            "-ERR unknown command 'NOTACOMMAND', with args beginning with: "
            "\r\n");
  const std::vector<std::string_view> args = {"a", "b c"};
  ASSERT_EQ(UnknownCommand("foo", args),
            "-ERR unknown command 'foo', with args beginning with: 'a' 'b c' "
            "\r\n");
}

TEST(ReplyTest, UnknownCommandMatchesRedisTruncation) {
  // Redis quotes the name up to 128 bytes, and arguments until 128 bytes of
  // the list are used, cutting the argument that crosses the limit.
  const std::string long_name(200, 'n');
  const std::string long_arg(200, 'x');
  const std::vector<std::string_view> args = {"ab", long_arg, "unused"};
  const std::string expected = "-ERR unknown command '" +
                               std::string(128, 'n') +
                               "', with args beginning with: 'ab' '" +
                               std::string(128 - 5, 'x') + "' \r\n";
  EXPECT_EQ(UnknownCommand(long_name, args), expected);
  // Newlines from the client cannot end the error line early.
  const std::vector<std::string_view> newline = {"a\r\nb"};
  EXPECT_EQ(UnknownCommand("x\ny", newline),
            "-ERR unknown command 'x y', with args beginning with: 'a  b' "
            "\r\n");
}
TEST(ReplyDeathTest, FromArrayRequiresEncodedElements) {
  EXPECT_DEATH(FromArray({":123\r\n", "\r", "$13\r\nhello world"}),
               "Check failed");
  EXPECT_DEATH(
      FromArray({":123\r\n", "+hello world\r\n", "$13\r\nhello world"}),
      "Check failed");
}
}  // namespace redis_simple::reply
