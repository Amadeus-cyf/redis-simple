#include "server/reply.h"

#include <gtest/gtest.h>

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
  EXPECT_DEATH(FromArray({":123\r\n", "\r", "$13\r\nhello world"}),
               "Check failed");
  EXPECT_DEATH(
      FromArray({":123\r\n", "+hello world\r\n", "$13\r\nhello world"}),
      "Check failed");
}

TEST(ReplyTest, FromError) {
  ASSERT_EQ(FromError("ERR syntax error"), "-ERR syntax error\r\n");
  ASSERT_EQ(WrongNumberOfArguments(), "-ERR wrong number of arguments\r\n");
  ASSERT_EQ(UnknownCommand("NOTACOMMAND"),
            "-ERR unknown command 'NOTACOMMAND'\r\n");
}
}  // namespace redis_simple::reply
