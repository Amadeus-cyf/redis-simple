#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/cli.h"
#include "integration/client_helpers.h"
#include "logging/logger.h"

namespace redis_simple {
namespace {
using integration::Case;
using integration::ExpectLines;
using integration::ExpectReply;
using integration::NonEmptyLines;
using integration::ReplyLines;
using integration::ScanAll;

bool ExpectMembersInOrder(cli::RedisCli* cli, const std::string& command,
                          const std::vector<std::string>& expected_members) {
  cli->AddCommand(command);
  const std::vector<std::string> actual_members =
      NonEmptyLines(cli->ReadReply());
  if (actual_members != expected_members) {
    RS_LOG_DEBUG("member command failed: %s\n", command.c_str());
    return false;
  }
  return true;
}

bool ExpectZScan() {
  cli::RedisCli cli;
  if (cli.Connect("localhost", 8080) == cli::CliStatus::kError) {
    return false;
  }
  const std::string wrong_type =
      "WRONGTYPE Operation against a key holding the wrong kind of value\n";
  const std::vector<Case> cases = {
      {"ZADD scan_zset 1.1 a 2 b -0.5 c inf d\r\n", "4\n"},
      {"SET scan_string value\r\n", "OK\n"},
      {"ZSCAN scan_string 0\r\n", wrong_type},
      {"ZSCAN scan_zset 18446744073709551616\r\n", "ERR invalid cursor\n"},
      {"ZSCAN scan_zset 0 NOVALUES\r\n",
       "ERR NOVALUES option can only be used in HSCAN\n"},
      {"ZSCAN scan_zset 0 COUNT -1\r\n", "ERR syntax error\n"},
  };
  for (const Case& test_case : cases) {
    if (!ExpectReply(&cli, test_case)) {
      return false;
    }
  }
  // A listpack is returned whole with cursor 0, in score order, with scores
  // printed as ZSCORE prints them.
  if (!ExpectLines(&cli,
                   {{"ZSCAN", "missing_zset", "0"},
                    {"ZSCAN", "scan_zset", "0"},
                    {"ZSCAN", "scan_zset", "0", "MATCH", "[ab]"}},
                   {{"0"},
                    {"0", "c", "-0.5", "a", "1.1", "b", "2", "d", "inf"},
                    {"0", "a", "1.1", "b", "2"}})) {
    return false;
  }

  // Past 128 members the sorted set is a skiplist, scanned in steps, and
  // Redis prints its scores with 17 significant digits.
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 200; ++i) {
    const std::string member = "member:" + std::to_string(i);
    const std::string score = std::to_string(i);
    cli.AddCommand(
        std::vector<std::string_view>{"ZADD", "scan_big_zset", score, member});
    cli.ReadReply();
    entries.emplace_back(member, score);
  }
  cli.AddCommand(std::vector<std::string_view>{"ZADD", "scan_big_zset", "1.1",
                                               "member:1"});
  cli.ReadReply();
  entries[1].second = "1.1000000000000001";
  std::ranges::sort(entries);
  const std::vector<std::string> scanned =
      ScanAll(&cli, "ZSCAN", "scan_big_zset", {"COUNT", "5"});
  std::vector<std::pair<std::string, std::string>> pairs;
  for (size_t i = 0; i + 1 < scanned.size(); i += 2) {
    pairs.emplace_back(scanned[i], scanned[i + 1]);
  }
  std::ranges::sort(pairs);
  if (pairs != entries) {
    RS_LOG_DEBUG("ZSCAN did not return every member exactly once\n");
    return false;
  }
  return true;
}
}  // namespace

int Run() {
  cli::RedisCli cli;
  if (cli.Connect("localhost", 8080) == cli::CliStatus::kError) {
    RS_LOG_DEBUG("failed to connect to integration server\n");
    return EXIT_FAILURE;
  }

  const std::vector<Case> cases = {
      {"ZADD integration_zset 3.0 ele1\r\n", "1\n"},
      {"ZADD integration_zset 1 ele1\r\n", "0\n"},
      {"ZADD integration_zset 1.0000234 ele2\r\n", "1\n"},
      {"ZRANK integration_zset ele1\r\n", "0\n"},
      {"ZRANK integration_zset ele2\r\n", "1\n"},
      {"ZRANK integration_zset ele3\r\n", "(nil)\n"},
      {"ZCARD integration_zset\r\n", "2\n"},
      {"ZCARD missing_zset\r\n", "0\n"},
      {"ZREM missing_zset ele1 ele2\r\n", "0\n"},
      {"ZADD emptied_zset 1 only\r\n", "1\n"},
      {"ZREM emptied_zset only\r\n", "1\n"},
      {"EXISTS emptied_zset\r\n", "0\n"},
      {"ZCOUNT integration_zset 1 2\r\n", "2\n"},
      {"ZCOUNT missing_zset 1 2\r\n", "0\n"},
      {"SET zset_wrong_type value\r\n", "OK\n"},
      {"ZCARD zset_wrong_type\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
      {"ZRANGE integration_zset 0 1 WTHSCORES\r\n", "ERR syntax error\n"},
      {"ZRANGE integration_zset 0 1 LIMIT 0\r\n", "ERR syntax error\n"},
      {"ZRANGE integration_zset 0 1 LIMIT -1 1\r\n",
       "ERR syntax error, LIMIT is only supported in combination with either "
       "BYSCORE or BYLEX\n"},
      {"ZRANGE integration_zset (0 1\r\n",
       "ERR value is not an integer or out of range\n"},
      {"ZADD integration_zset 1junk invalid\r\n",
       "ERR value is not a valid float\n"},
      {"ZADD integration_zset nan invalid\r\n",
       "ERR value is not a valid float\n"},
      {"ZRANGE integration_zset 1junk 2 BYSCORE\r\n",
       "ERR min or max is not a float\n"},
      {"ZCARD integration_zset\r\n", "2\n"},
      // Like Redis, the listpack encoding stores -0 as the integer 0.
      {"ZADD zset_negative_zero -0 member\r\n", "1\n"},
      {"ZSCORE zset_negative_zero member\r\n", "0\n"},
      // ZADD options, checked in Redis' order.
      {"ZADD zflags NX 1 a 2 b\r\n", "2\n"},
      {"ZADD zflags GT CH\r\n", "ERR syntax error\n"},
      {"ZADD zflags XX NX 1 a\r\n",
       "ERR XX and NX options at the same time are not compatible\n"},
      {"ZADD zflags GT LT 1 a\r\n",
       "ERR GT, LT, and/or NX options at the same time are not compatible\n"},
      {"ZADD zflags INCR 1 a 1 b\r\n",
       "ERR INCR option supports a single increment-element pair\n"},
      {"ZADD zflags XX CH 5 a 9 missing\r\n", "1\n"},
      {"ZADD zflags GT CH 1 b\r\n", "0\n"},
      {"ZADD zflags LT CH 1 b\r\n", "1\n"},
      {"ZADD zflags INCR 2 a\r\n", "7\n"},
      {"ZADD zflags NX INCR 2 a\r\n", "(nil)\n"},
      {"ZCARD zflags\r\n", "2\n"},
      {"ZREVRANK zflags a\r\n", "0\n"},
      {"ZRANK zflags missing WITHSCORE\r\n", "(nil)\n"},
      {"ZRANK zflags a WITHSCOR\r\n", "ERR syntax error\n"},
      // Numbers as Redis 8 parses and prints them.
      {"ZADD znumbers 0x10 hex 1e-5 small 1e17 big\r\n", "3\n"},
      {"ZSCORE znumbers hex\r\n", "16\n"},
      {"ZSCORE znumbers small\r\n", "0.00001\n"},
      {"ZSCORE znumbers big\r\n", "100000000000000000\n"},
      {"ZADD znumbers 1e400 overflow\r\n", "ERR value is not a valid float\n"},
      {"ZCOUNT znumbers 1e400 +inf\r\n", "0\n"},
      // Lexicographic ranges.
      {"ZADD zlex 0 a 0 b 0 c 0 d\r\n", "4\n"},
      {"ZLEXCOUNT zlex - +\r\n", "4\n"},
      {"ZLEXCOUNT zlex [b (d\r\n", "2\n"},
      {"ZRANGEBYLEX zlex a c\r\n",
       "ERR min or max not valid string range item\n"},
      {"ZRANGE zlex - + BYLEX WITHSCORES\r\n",
       "ERR syntax error, WITHSCORES not supported in combination with "
       "BYLEX\n"},
      // Only ZRANGE takes REV, BYSCORE, and BYLEX.
      {"ZRANGEBYSCORE zlex 0 1 REV\r\n", "ERR syntax error\n"},
  };
  for (const Case& test_case : cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  if (!ExpectMembersInOrder(&cli, "ZRANGE integration_zset 0 1\r\n",
                            {"ele1", "ele2"})) {
    return EXIT_FAILURE;
  }
  for (const auto& [command, members] :
       std::vector<std::pair<std::string, std::vector<std::string>>>{
           {"ZRANGEBYLEX zlex [b (d\r\n", {"b", "c"}},
           {"ZREVRANGEBYLEX zlex + - LIMIT 0 2\r\n", {"d", "c"}},
           {"ZRANGE zlex + [b BYLEX REV\r\n", {"d", "c", "b"}},
           {"ZREVRANGEBYSCORE zflags +inf -inf WITHSCORES\r\n",
            {"a", "7", "b", "1"}},
           {"ZRANK zflags a WITHSCORE\r\n", {"1", "7"}},
       }) {
    if (!ExpectMembersInOrder(&cli, command, members)) {
      return EXIT_FAILURE;
    }
  }
  // Like Redis, a rank range ignores LIMIT with a count of -1.
  if (!ExpectMembersInOrder(&cli, "ZRANGE integration_zset 0 1 LIMIT 0 -1\r\n",
                            {"ele1", "ele2"})) {
    return EXIT_FAILURE;
  }
  // A negative LIMIT offset selects nothing.
  if (!ExpectMembersInOrder(
          &cli, "ZRANGE integration_zset 1 2 BYSCORE LIMIT -1 1\r\n", {})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembersInOrder(
          &cli, "ZRANGE integration_zset 1 2 BYSCORE LIMIT 0 -1\r\n",
          {"ele1", "ele2"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembersInOrder(&cli, "ZRANGE integration_zset 1.0 2.0 BYSCORE\r\n",
                            {"ele1", "ele2"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembersInOrder(&cli, "ZREVRANGE integration_zset 0 1\r\n",
                            {"ele2", "ele1"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembersInOrder(&cli, "ZRANGE integration_zset 2 1 REV BYSCORE\r\n",
                            {"ele2", "ele1"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembersInOrder(&cli, "ZRANGEBYSCORE integration_zset 1.0 2.0\r\n",
                            {"ele1", "ele2"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembersInOrder(&cli, "ZRANGE integration_zset -10 1\r\n",
                            {"ele1", "ele2"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"ZRANGE integration_zset 0 1 WITHSCORES\r\n",
                          "ele1\n1\nele2\n1.0000234\n\n\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"ZRANGE integration_zset 1 2 byscore withscores\r\n",
                          "ele1\n1\nele2\n1.0000234\n\n\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembersInOrder(&cli, "ZRANGE missing_zset 0 -1\r\n", {})) {
    return EXIT_FAILURE;
  }

  const std::vector<Case> remove_cases = {
      {"ZREM integration_zset ele1\r\n", "1\n"},
      {"ZREM integration_zset ele1\r\n", "0\n"},
      {"ZRANK integration_zset ele1\r\n", "(nil)\n"},
      {"ZRANK integration_zset ele2\r\n", "0\n"},
      {"ZCARD integration_zset\r\n", "1\n"},
      {"ZSCORE integration_zset ele1\r\n", "(nil)\n"},
      {"ZSCORE missing_zset ele2\r\n", "(nil)\n"},
  };
  for (const Case& test_case : remove_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }
  return ExpectZScan() ? EXIT_SUCCESS : EXIT_FAILURE;
}
}  // namespace redis_simple

int main() {
  try {
    return redis_simple::Run();
  } catch (...) {
    return EXIT_FAILURE;
  }
}
