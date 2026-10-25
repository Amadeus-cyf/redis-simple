#include "server/request_parser.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace redis_simple::request_parser {
TEST(RequestParserTest, ParsesRespBulkStringArrayWithoutCopying) {
  constexpr std::string_view kRequest =
      "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$11\r\nhello world\r\n";
  std::string_view command;
  command::CommandArgs args;

  const auto result = Parse(kRequest, &command, &args);

  EXPECT_EQ(result.status, ParseStatus::kComplete);
  EXPECT_EQ(result.consumed, kRequest.size());
  EXPECT_EQ(command, "SET");
  ASSERT_EQ(args.size(), 2);
  EXPECT_EQ(args[0], "key");
  EXPECT_EQ(args[1], "hello world");
  EXPECT_GE(command.data(), kRequest.data());
  EXPECT_LT(command.data(), kRequest.data() + kRequest.size());
}

TEST(RequestParserTest, DistinguishesIncompleteAndInvalidResp) {
  std::string_view command;
  command::CommandArgs args;

  EXPECT_EQ(Parse("*2\r\n$3\r\nGET\r\n$3\r\nke", &command, &args).status,
            ParseStatus::kIncomplete);
  EXPECT_TRUE(command.empty());
  EXPECT_TRUE(args.empty());
  EXPECT_EQ(Parse("*1\r\n+GET\r\n", &command, &args).status,
            ParseStatus::kInvalid);
  EXPECT_EQ(Parse("*0\r\n", &command, &args).status, ParseStatus::kInvalid);
}

TEST(RequestParserTest, BoundsReservationForIncompleteResp) {
  constexpr std::string_view kRequest = "*1048576\r\n";
  std::string_view command;
  command::CommandArgs args;

  EXPECT_EQ(Parse(kRequest, &command, &args).status, ParseStatus::kIncomplete);
  EXPECT_LE(args.capacity(), kRequest.size());
}

TEST(RequestParserTest, ResumesSplitRequestFromSavedState) {
  const std::string request =
      "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$11\r\nhello world\r\n";
  ParseState state;
  std::string_view command;
  command::CommandArgs args;

  // Feed the request one byte at a time, copying it to a fresh buffer each
  // time so saved state cannot depend on stable addresses.
  for (size_t size = 1; size < request.size(); ++size) {
    const std::string partial = request.substr(0, size);
    ASSERT_EQ(Parse(partial, &state, &command, &args).status,
              ParseStatus::kIncomplete)
        << size;
    EXPECT_TRUE(args.empty());
  }
  const auto result = Parse(request, &state, &command, &args);
  ASSERT_EQ(result.status, ParseStatus::kComplete);
  EXPECT_EQ(result.consumed, request.size());
  EXPECT_EQ(command, "SET");
  ASSERT_EQ(args.size(), 2);
  EXPECT_EQ(args[0], "key");
  EXPECT_EQ(args[1], "hello world");
  EXPECT_EQ(state.element_count, 0);
}

TEST(RequestParserTest, ReportsPendingBulkBytes) {
  constexpr std::string_view kRequest = "*2\r\n$3\r\nSET\r\n$100\r\nabc";
  ParseState state;
  std::string_view command;
  command::CommandArgs args;

  ASSERT_EQ(Parse(kRequest, &state, &command, &args).status,
            ParseStatus::kIncomplete);
  // 100 payload bytes plus CRLF, of which 3 have arrived.
  EXPECT_EQ(state.PendingBulkBytes(kRequest.size()), 99);
}

TEST(RequestParserTest, RejectsOverlongHeaderLines) {
  std::string_view command;
  command::CommandArgs args;

  const std::string long_count = "*" + std::string(size_t{70} * 1024, '1');
  EXPECT_EQ(Parse(long_count, &command, &args).status, ParseStatus::kInvalid);
  const std::string long_inline(size_t{70} * 1024, 'a');
  EXPECT_EQ(Parse(long_inline, &command, &args).status, ParseStatus::kInvalid);
  const std::string short_inline(1024, 'a');
  EXPECT_EQ(Parse(short_inline, &command, &args).status,
            ParseStatus::kIncomplete);
}

TEST(RequestParserTest, PreservesInlineProtocolCompatibility) {
  std::string_view command;
  command::CommandArgs args;

  const auto result = Parse("  GET   key  \r\nremaining", &command, &args);

  EXPECT_EQ(result.status, ParseStatus::kComplete);
  EXPECT_EQ(result.consumed, 15);
  EXPECT_EQ(command, "GET");
  ASSERT_EQ(args.size(), 1);
  EXPECT_EQ(args[0], "key");
}
}  // namespace redis_simple::request_parser
