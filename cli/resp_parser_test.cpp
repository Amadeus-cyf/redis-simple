#include "cli/resp_parser.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::cli::resp_parser {
namespace {
std::ptrdiff_t ParseBytes(std::string_view response,
                          std::vector<std::string>& reply) {
  const auto result = Parse(response, reply);
  return result.status == ParseStatus::kComplete
             ? static_cast<std::ptrdiff_t>(result.consumed)
             : -1;
}
}  // namespace

TEST(RespParserTest, ParseString) {
  std::vector<std::string> reply;
  ASSERT_EQ(ParseBytes("+OK\r\n", reply), 5);
  ASSERT_EQ(reply.back(), "OK");
  ASSERT_EQ(ParseBytes("+123_string\r\n", reply), 13);
  ASSERT_EQ(reply.back(), "123_string");
  ASSERT_EQ(ParseBytes("+\r\n", reply), 3);
  ASSERT_EQ(reply.back(), "");
  ASSERT_EQ(ParseBytes("+OK", reply), -1);
  ASSERT_EQ(ParseBytes("+hello\n\r\n", reply), 9);
  ASSERT_EQ(reply.back(), "hello\n");
}

TEST(RespParserTest, DistinguishesIncompleteAndInvalidFrames) {
  std::vector<std::string> reply;

  EXPECT_EQ(Parse("*2\r\n$3\r\none\r\n", reply).status,
            ParseStatus::kIncomplete);
  EXPECT_TRUE(reply.empty());
  EXPECT_EQ(Parse("$3\r\noneXX", reply).status, ParseStatus::kInvalid);
  EXPECT_TRUE(reply.empty());
}

TEST(RespParserTest, ParseBulkString) {
  std::vector<std::string> reply;
  ASSERT_EQ(ParseBytes("$5\r\nhello\r\n", reply), 11);
  ASSERT_EQ(reply.back(), "hello");
  ASSERT_EQ(ParseBytes("$7\r\nhello\r\n\r\n", reply), 13);
  ASSERT_EQ(reply.back(), "hello\r\n");
  ASSERT_EQ(ParseBytes("$5\r\n", reply), -1);
  ASSERT_EQ(ParseBytes("$5hello\r\n", reply), -1);
  ASSERT_EQ(ParseBytes("$5\r\nhello", reply), -1);
  ASSERT_EQ(ParseBytes("$\r\n\r\n", reply), -1);
  ASSERT_EQ(ParseBytes("$6\r\nhello\n\r\n", reply), 12);
  ASSERT_EQ(reply.back(), "hello\n");
  ASSERT_EQ(ParseBytes("$15\r\nhello_hello_hel\r\n\r\n\r\n", reply), 22);
  ASSERT_EQ(reply.back(), "hello_hello_hel");
  ASSERT_EQ(ParseBytes("$-1\r\n", reply), 5);
  ASSERT_EQ(reply.back(), "(nil)");
}

TEST(RespParserTest, ParseInt64) {
  std::vector<std::string> reply;
  ASSERT_EQ(ParseBytes(":123456\r\n", reply), 9);
  ASSERT_EQ(reply.back(), "123456");
  ASSERT_EQ(ParseBytes(":+1234567\r\n", reply), 11);
  ASSERT_EQ(reply.back(), "1234567");
  ASSERT_EQ(ParseBytes(":-123456\r\n", reply), 10);
  ASSERT_EQ(reply.back(), "-123456");
  ASSERT_EQ(ParseBytes(":--123456\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(":\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(":+\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(":123456", reply), -1);
}

TEST(ReplyParserTest, ParseArray) {
  // Base
  std::vector<std::string> reply1;
  const std::string& s1 =
      "*5\r\n+key1\r\n$5\r\nhello\r\n$7\r\nhello\r\n\r\n:123456\r\n:-12345\r\n";
  ASSERT_EQ(ParseBytes(s1, reply1), s1.size());
  ASSERT_EQ(reply1, std::vector<std::string>({"key1", "hello", "hello\r\n",
                                              "123456", "-12345", "\n"}));
  // Empty array
  std::vector<std::string> reply2;
  const std::string& s2 = "*0\r\n";
  ASSERT_EQ(ParseBytes(s2, reply2), 4);
  ASSERT_EQ(reply2, std::vector<std::string>({"\n"}));
  ASSERT_EQ(ParseBytes("*\r\n", reply2), -1);
  // Nested array
  std::vector<std::string> reply3;
  const std::string& s3 =
      "*3\r\n*5\r\n+key1\r\n$5\r\nhello\r\n$7\r\nhello\r\n\r\n:123456\r\n:-"
      "12345\r\n*0\r\n*3\r\n+key2\r\n$7\r\nhello\r\n\r\n:123456\r\n";
  ASSERT_EQ(ParseBytes(s3, reply3), s3.size());
  ASSERT_EQ(reply3, std::vector<std::string>(
                        {"key1", "hello", "hello\r\n", "123456", "-12345", "\n",
                         "\n", "key2", "hello\r\n", "123456", "\n", "\n"}));
  // Multi-digit array lengths should consume every digit in the length header.
  std::vector<std::string> reply4;
  const std::string& s4 =
      "*10\r\n:0\r\n:1\r\n:2\r\n:3\r\n:4\r\n:5\r\n:6\r\n:7\r\n:8\r\n:9\r\n";
  ASSERT_EQ(ParseBytes(s4, reply4), s4.size());
  ASSERT_EQ(reply4, std::vector<std::string>({"0", "1", "2", "3", "4", "5", "6",
                                              "7", "8", "9", "\n"}));
}

TEST(ReplyParserTest, ParseMap) {
  std::vector<std::string> reply;
  constexpr std::string_view kResponse =
      "%2\r\n$6\r\nserver\r\n$12\r\nredis_simple\r\n$5\r\nproto\r\n:3\r\n";

  ASSERT_EQ(ParseBytes(kResponse, reply), kResponse.size());
  EXPECT_EQ(reply, std::vector<std::string>(
                       {"server", "redis_simple", "proto", "3", "\n"}));
}

TEST(ReplyParserTest, ParseSet) {
  std::vector<std::string> reply;
  constexpr std::string_view kResponse = "~2\r\n$3\r\none\r\n$3\r\ntwo\r\n";

  ASSERT_EQ(ParseBytes(kResponse, reply), kResponse.size());
  EXPECT_EQ(reply, std::vector<std::string>({"one", "two", "\n"}));
}

TEST(ReplyParserTest, ParseNullArray) {
  std::vector<std::string> reply;
  ASSERT_EQ(ParseBytes("*-1\r\n", reply), 5);
  ASSERT_EQ(reply.back(), "(nil)");
  ASSERT_EQ(ParseBytes("~-1\r\n", reply), -1);
}

TEST(ReplyParseTest, ParseNull) {
  std::vector<std::string> reply;
  ASSERT_EQ(ParseBytes("_\r\n", reply), 3);
  ASSERT_EQ(reply.back(), "(nil)");
  ASSERT_EQ(ParseBytes("_123\r\n", reply), -1);
}

TEST(ReplyParserTest, ParseFloat) {
  std::vector<std::string> reply;
  ASSERT_EQ(ParseBytes(",123.000002300000\r\n", reply), 19);
  ASSERT_EQ(reply.back(), "123.0000023");
  ASSERT_EQ(ParseBytes(",-123.000230000\r\n", reply), 17);
  ASSERT_EQ(reply.back(), "-123.00023");
  ASSERT_EQ(ParseBytes(",123.456e12\r\n", reply), 13);
  ASSERT_EQ(reply.back(), "123.456e12");
  ASSERT_EQ(ParseBytes(",-123.456e12\r\n", reply), 14);
  ASSERT_EQ(reply.back(), "-123.456e12");
  ASSERT_EQ(ParseBytes(",-123.456e-12\r\n", reply), 15);
  ASSERT_EQ(reply.back(), "-123.456e-12");
  ASSERT_EQ(ParseBytes(",.456\r\n", reply), 7);
  ASSERT_EQ(reply.back(), "0.456");
  ASSERT_EQ(ParseBytes(",0.4567\r\n", reply), 9);
  ASSERT_EQ(reply.back(), "0.4567");
  ASSERT_EQ(ParseBytes(",.456e12\r\n", reply), 10);
  ASSERT_EQ(reply.back(), "0.456e12");
  ASSERT_EQ(ParseBytes(",+123.456e+4567\r\n", reply), 17);
  ASSERT_EQ(reply.back(), "123.456e4567");
  ASSERT_EQ(ParseBytes(",+123.456e-4567\r\n", reply), 17);
  ASSERT_EQ(reply.back(), "123.456e-4567");
  ASSERT_EQ(ParseBytes(",-123.456e+4567\r\n", reply), 17);
  ASSERT_EQ(reply.back(), "-123.456e4567");
  ASSERT_EQ(ParseBytes(",+123\r\n", reply), 7);
  ASSERT_EQ(reply.back(), "123");
  ASSERT_EQ(ParseBytes(",+12.\r\n", reply), 7);
  ASSERT_EQ(reply.back(), "12");
  ASSERT_EQ(ParseBytes(",+12.e14\r\n", reply), 10);
  ASSERT_EQ(reply.back(), "12e14");
  ASSERT_EQ(ParseBytes(",+123e12\r\n", reply), 10);
  ASSERT_EQ(reply.back(), "123e12");
  ASSERT_EQ(ParseBytes(",-123\r\n", reply), 7);
  ASSERT_EQ(reply.back(), "-123");
  ASSERT_EQ(ParseBytes(",-1234.\r\n", reply), 9);
  ASSERT_EQ(reply.back(), "-1234");
  ASSERT_EQ(ParseBytes(",-123.e12\r\n", reply), 11);
  ASSERT_EQ(reply.back(), "-123e12");
  ASSERT_EQ(ParseBytes(",123.456e\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(",-123.456e\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(",123.456e+\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(",123.456e-\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(",123.2.34\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(",123.34e1.23\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(",123.2abc34\r\n", reply), -1);
  ASSERT_EQ(ParseBytes(",e123.34\r\n", reply), -1);
}
}  // namespace redis_simple::cli::resp_parser
