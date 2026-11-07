#include "utils/split_args.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::utils {
namespace {
struct Argument {
  std::string value;
  bool quoted;

  bool operator==(const Argument&) const = default;
};

std::optional<std::vector<Argument>> Split(std::string_view line) {
  std::vector<Argument> arguments;
  if (!SplitArgs(line, [&arguments](std::string_view value, bool quoted) {
        arguments.push_back({std::string(value), quoted});
      })) {
    return std::nullopt;
  }
  return arguments;
}

TEST(SplitArgsTest, SplitsOnWhitespace) {
  EXPECT_EQ(Split("SET key value"),
            (std::vector<Argument>{
                {"SET", false}, {"key", false}, {"value", false}}));
  EXPECT_EQ(Split("  a\t\tb \r\n"),
            (std::vector<Argument>{{"a", false}, {"b", false}}));
  EXPECT_EQ(Split(""), std::vector<Argument>{});
  EXPECT_EQ(Split(" \t "), std::vector<Argument>{});
  // \v and \f separate arguments only before one starts, as in Redis.
  EXPECT_EQ(Split("\va\vb"), (std::vector<Argument>{{"a\vb", false}}));
}

TEST(SplitArgsTest, UnescapesDoubleQuotes) {
  EXPECT_EQ(Split(R"("a b" "c\"d" "\x41\x6a" "\n\r\t\b\a\\" "\q")"),
            (std::vector<Argument>{{"a b", true},
                                   {"c\"d", true},
                                   {"Aj", true},
                                   {"\n\r\t\b\a\\", true},
                                   {"q", true}}));
  // An incomplete \x escape keeps the x.
  EXPECT_EQ(Split(R"("\xZ1")"), (std::vector<Argument>{{"xZ1", true}}));
  EXPECT_EQ(Split(R"("")"), (std::vector<Argument>{{"", true}}));
}

TEST(SplitArgsTest, KeepsSingleQuotesLiteral) {
  EXPECT_EQ(Split(R"('a "b" \n' 'it\'s')"),
            (std::vector<Argument>{{R"(a "b" \n)", true}, {"it's", true}}));
}

TEST(SplitArgsTest, JoinsTextBeforeAQuote) {
  EXPECT_EQ(Split(R"(foo"bar baz" x'y')"),
            (std::vector<Argument>{{"foobar baz", true}, {"xy", true}}));
}

TEST(SplitArgsTest, RejectsUnbalancedQuotes) {
  EXPECT_EQ(Split(R"("abc)"), std::nullopt);
  EXPECT_EQ(Split("'abc"), std::nullopt);
  EXPECT_EQ(Split(R"("abc"def)"), std::nullopt);
  EXPECT_EQ(Split(R"('abc'"def")"), std::nullopt);
  EXPECT_EQ(Split(R"("abc\)"), std::nullopt);
}

TEST(SplitArgsTest, StopsAtNul) {
  EXPECT_EQ(Split(std::string_view("a b\0c d", 7)),
            (std::vector<Argument>{{"a", false}, {"b", false}}));
  EXPECT_EQ(Split(std::string_view("\"a\0\"", 4)), std::nullopt);
}
}  // namespace
}  // namespace redis_simple::utils
