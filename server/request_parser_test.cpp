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
  commands::CommandArgs args;

  const ParseResult result = Parse(kRequest, &command, &args);

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
  commands::CommandArgs args;

  EXPECT_EQ(Parse("*2\r\n$3\r\nGET\r\n$3\r\nke", &command, &args).status,
            ParseStatus::kIncomplete);
  EXPECT_TRUE(command.empty());
  EXPECT_TRUE(args.empty());
  const ParseResult invalid = Parse("*1\r\n+GET\r\n", &command, &args);
  EXPECT_EQ(invalid.status, ParseStatus::kInvalid);
  EXPECT_EQ(ProtocolErrorMessage(invalid),
            "Protocol error: expected '$', got '+'");
}

TEST(RequestParserTest, SkipsEmptyAndNullArraysLikeRedis) {
  std::string_view command;
  commands::CommandArgs args;
  for (const std::string_view request : {"*0\r\n", "*-1\r\n", "*-5\r\n"}) {
    const ParseResult result = Parse(request, &command, &args);
    EXPECT_EQ(result.status, ParseStatus::kComplete) << request;
    EXPECT_TRUE(result.empty_request) << request;
    EXPECT_EQ(result.consumed, request.size()) << request;
  }
}

TEST(RequestParserTest, ValidatesLengthFieldsLikeRedis) {
  std::string_view command;
  commands::CommandArgs args;
  const auto parse = [&](std::string_view input) {
    return Parse(input, &command, &args);
  };

  for (const std::string_view partial :
       {"*", "*1", "*1\r", "*1\r\n$", "*1\r\n$3", "*1\r\n$3\r",
        "*1\r\n$3\r\nGET\r"}) {
    EXPECT_EQ(parse(partial).status, ParseStatus::kIncomplete) << partial;
  }
  // Lengths are string2ll text: no sign but '-', no spaces or leading zeros.
  for (const std::string_view invalid :
       {"*\r\n", "*+1\r\n", "*1 \r\n", "*01\r\n", "*2147483648\r\n"}) {
    const ParseResult result = parse(invalid);
    EXPECT_EQ(result.status, ParseStatus::kInvalid) << invalid;
    EXPECT_EQ(ProtocolErrorMessage(result),
              "Protocol error: invalid multibulk length")
        << invalid;
  }
  // Bulk lengths are at most Redis' proto-max-bulk-len, 512 MB.
  for (const std::string_view invalid :
       {"*1\r\n$\r\n", "*1\r\n$-1\r\n", "*1\r\n$3x\r\nGET\r\n",
        "*1\r\n$003\r\nGET\r\n", "*1\r\n$536870913\r\n",
        "*1\r\n$99999999999999999999\r\n"}) {
    const ParseResult result = parse(invalid);
    EXPECT_EQ(result.status, ParseStatus::kInvalid) << invalid;
    EXPECT_EQ(ProtocolErrorMessage(result),
              "Protocol error: invalid bulk length")
        << invalid;
  }
  EXPECT_EQ(parse("*1\r\n$536870912\r\n").status, ParseStatus::kIncomplete);
  // Like Redis, the bytes after a CR and after a payload are taken as line
  // endings without checking them.
  EXPECT_EQ(parse("*1\rx$3\rxGETxx").status, ParseStatus::kComplete);
  EXPECT_EQ(command, "GET");
  // Long length lines wait for more bytes, up to 64 KB.
  EXPECT_EQ(parse("*" + std::string(1000, '0')).status,
            ParseStatus::kIncomplete);
}

TEST(RequestParserTest, LimitsUnauthenticatedRequestsLikeRedis) {
  std::string_view command;
  commands::CommandArgs args;
  ParseState state;
  state.authenticated = false;
  const ParseResult many = Parse("*11\r\n", &state, &command, &args);
  EXPECT_EQ(ProtocolErrorMessage(many),
            "Protocol error: unauthenticated multibulk length");
  state.Reset();
  const ParseResult big = Parse("*1\r\n$16385\r\n", &state, &command, &args);
  EXPECT_EQ(ProtocolErrorMessage(big),
            "Protocol error: unauthenticated bulk length");
}

TEST(RequestParserTest, BoundsReservationForIncompleteResp) {
  constexpr std::string_view kRequest = "*1048576\r\n";
  std::string_view command;
  commands::CommandArgs args;

  EXPECT_EQ(Parse(kRequest, &command, &args).status, ParseStatus::kIncomplete);
  EXPECT_LE(args.capacity(), kRequest.size());
}

TEST(RequestParserTest, ResumesSplitRequestFromSavedState) {
  const std::string request =
      "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$11\r\nhello world\r\n";
  ParseState state;
  std::string_view command;
  commands::CommandArgs args;

  // Feed the request one byte at a time, copying it to a fresh buffer each
  // time so saved state cannot depend on stable addresses.
  for (size_t size = 1; size < request.size(); ++size) {
    const std::string partial = request.substr(0, size);
    ASSERT_EQ(Parse(partial, &state, &command, &args).status,
              ParseStatus::kIncomplete)
        << size;
    EXPECT_TRUE(args.empty());
  }
  const ParseResult result = Parse(request, &state, &command, &args);
  ASSERT_EQ(result.status, ParseStatus::kComplete);
  EXPECT_EQ(result.consumed, request.size());
  EXPECT_EQ(command, "SET");
  ASSERT_EQ(args.size(), 2);
  EXPECT_EQ(args[0], "key");
  EXPECT_EQ(args[1], "hello world");
  EXPECT_EQ(state.element_count, 0);
}

TEST(RequestParserTest, HandsOffFromAnySplitPointToResume) {
  constexpr std::string_view kRequest =
      "*4\r\n$4\r\nHSET\r\n$1\r\nh\r\n$5\r\nfield\r\n$11\r\nhello "
      "world\r\n";
  for (size_t split = 1; split < kRequest.size(); ++split) {
    ParseState state;
    std::string_view command;
    commands::CommandArgs args;
    const std::string first(kRequest.substr(0, split));
    ASSERT_EQ(Parse(first, &state, &command, &args).status,
              ParseStatus::kIncomplete)
        << split;
    EXPECT_TRUE(command.empty()) << split;
    EXPECT_TRUE(args.empty()) << split;

    // The rest arrives in a separate buffer at a different address.
    const std::string full(kRequest);
    const ParseResult result = Parse(full, &state, &command, &args);
    ASSERT_EQ(result.status, ParseStatus::kComplete) << split;
    EXPECT_EQ(result.consumed, full.size());
    EXPECT_EQ(command, "HSET");
    ASSERT_EQ(args.size(), 3);
    EXPECT_EQ(args[0], "h");
    EXPECT_EQ(args[1], "field");
    EXPECT_EQ(args[2], "hello world");
    EXPECT_GE(command.data(), full.data());
    EXPECT_LT(args[2].data(), full.data() + full.size());
    EXPECT_EQ(state.element_count, 0);
  }

  // A large argument cut off on its first read still reports its pending
  // payload so the reader can read the rest in bigger steps.
  ParseState state;
  std::string_view command;
  commands::CommandArgs args;
  const std::string large_header = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$100000\r\n";
  const std::string partial = large_header + std::string(10, 'v');
  ASSERT_EQ(Parse(partial, &state, &command, &args).status,
            ParseStatus::kIncomplete);
  EXPECT_EQ(state.PendingBulkBytes(partial.size()), 100000 - 10 + 2);
}

TEST(RequestParserTest, ReportsPendingBulkBytes) {
  constexpr std::string_view kRequest = "*2\r\n$3\r\nSET\r\n$100\r\nabc";
  ParseState state;
  std::string_view command;
  commands::CommandArgs args;

  ASSERT_EQ(Parse(kRequest, &state, &command, &args).status,
            ParseStatus::kIncomplete);
  // 100 payload bytes plus CRLF, of which 3 have arrived.
  EXPECT_EQ(state.PendingBulkBytes(kRequest.size()), 99);
}

TEST(RequestParserTest, RejectsOverlongHeaderLines) {
  std::string_view command;
  commands::CommandArgs args;
  ParseState state;

  const std::string long_count = "*" + std::string(size_t{70} * 1024, '1');
  EXPECT_EQ(ProtocolErrorMessage(Parse(long_count, &command, &args)),
            "Protocol error: too big mbulk count string");
  const std::string long_bulk = "*1\r\n$" + std::string(size_t{70} * 1024, '1');
  EXPECT_EQ(ProtocolErrorMessage(Parse(long_bulk, &command, &args)),
            "Protocol error: too big bulk count string");
  const std::string long_inline(size_t{70} * 1024, 'a');
  EXPECT_EQ(ProtocolErrorMessage(Parse(long_inline, &state, &command, &args)),
            "Protocol error: too big inline request");
  const std::string short_inline(1024, 'a');
  EXPECT_EQ(Parse(short_inline, &state, &command, &args).status,
            ParseStatus::kIncomplete);
}

TEST(RequestParserTest, SplitsInlineRequestsLikeRedis) {
  std::string_view command;
  commands::CommandArgs args;
  ParseState state;

  ParseResult result =
      Parse("  GET   key  \r\nremaining", &state, &command, &args);
  EXPECT_EQ(result.status, ParseStatus::kComplete);
  EXPECT_EQ(result.consumed, 15);
  EXPECT_EQ(command, "GET");
  ASSERT_EQ(args.size(), 1);
  EXPECT_EQ(args[0], "key");

  // Quotes and escapes, as Redis' sdssplitargs reads them.
  result =
      Parse("set \"a b\\x41\\n\" 'it\\'s'\tc\"\"\n", &state, &command, &args);
  EXPECT_EQ(result.status, ParseStatus::kComplete);
  EXPECT_EQ(command, "set");
  ASSERT_EQ(args.size(), 3);
  EXPECT_EQ(args[0], "a bA\n");
  EXPECT_EQ(args[1], "it's");
  EXPECT_EQ(args[2], "c");

  for (const std::string_view unbalanced :
       {"get 'a\r\n", "get \"a\r\n", "get \"a\"b\r\n"}) {
    const ParseResult invalid = Parse(unbalanced, &state, &command, &args);
    EXPECT_EQ(ProtocolErrorMessage(invalid),
              "Protocol error: unbalanced quotes in request")
        << unbalanced;
  }

  // Empty lines are skipped.
  for (const std::string_view empty : {"\r\n", "\n", "   \t \r\n"}) {
    result = Parse(empty, &state, &command, &args);
    EXPECT_EQ(result.status, ParseStatus::kComplete) << empty;
    EXPECT_TRUE(result.empty_request) << empty;
  }
  state.ReleaseUnescaped();
}
}  // namespace redis_simple::request_parser
