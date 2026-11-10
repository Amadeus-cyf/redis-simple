#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/cli.h"
#include "logging/logger.h"

namespace redis_simple {
namespace {
struct Case {
  std::string command;
  std::string expected_reply;
};

std::vector<std::string> NonEmptyLines(const std::string& reply) {
  std::stringstream stream(reply);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

bool ExpectReply(cli::RedisCli* cli, const Case& test_case) {
  cli->AddCommand(test_case.command);
  const std::string reply = cli->ReadReply();
  if (reply != test_case.expected_reply) {
    RS_LOG_DEBUG("command failed: %s expected: %s actual: %s\n",
                 test_case.command.c_str(), test_case.expected_reply.c_str(),
                 reply.c_str());
    return false;
  }
  return true;
}

bool ExpectMembers(cli::RedisCli* cli, const std::string& command,
                   std::vector<std::string> expected_members) {
  cli->AddCommand(command);
  std::vector<std::string> actual_members = NonEmptyLines(cli->ReadReply());
  std::ranges::sort(actual_members);
  std::ranges::sort(expected_members);
  if (actual_members != expected_members) {
    RS_LOG_DEBUG("member command failed: %s\n", command.c_str());
    return false;
  }
  return true;
}
// Sends `command` and returns the non-empty lines of its reply.
std::vector<std::string> ReplyLines(
    cli::RedisCli* cli, const std::vector<std::string_view>& command) {
  cli->AddCommand(command);
  return NonEmptyLines(cli->ReadReply());
}

// Runs `command key cursor options...` from cursor 0 until the cursor
// returns to 0, and returns every element the steps returned.
std::vector<std::string> ScanAll(cli::RedisCli* cli, std::string_view command,
                                 std::string_view key,
                                 const std::vector<std::string_view>& options) {
  std::vector<std::string> elements;
  std::string cursor = "0";
  while (true) {
    std::vector<std::string_view> request = {command, key, cursor};
    request.insert(request.end(), options.begin(), options.end());
    std::vector<std::string> lines = ReplyLines(cli, request);
    if (lines.empty()) {
      return {};
    }
    cursor = lines[0];
    elements.insert(elements.end(), std::make_move_iterator(lines.begin() + 1),
                    std::make_move_iterator(lines.end()));
    if (cursor == "0") {
      return elements;
    }
  }
}

// Runs `requests` and compares each reply's lines, in order, to `expected`.
bool ExpectLines(cli::RedisCli* cli,
                 const std::vector<std::vector<std::string_view>>& requests,
                 const std::vector<std::vector<std::string>>& expected) {
  for (size_t i = 0; i < requests.size(); ++i) {
    if (ReplyLines(cli, requests[i]) != expected[i]) {
      RS_LOG_DEBUG("request %zu failed\n", i);
      return false;
    }
  }
  return true;
}

bool ExpectSScan() {
  cli::RedisCli cli;
  if (cli.Connect("localhost", 8080) == cli::CliStatus::kError) {
    return false;
  }
  const std::string wrong_type =
      "WRONGTYPE Operation against a key holding the wrong kind of value\n";
  const std::vector<Case> cases = {
      {"SADD scan_ints 5 3 -1 100\r\n", "4\n"},
      {"SADD scan_strings b a c 10\r\n", "4\n"},
      {"SET scan_string value\r\n", "OK\n"},
      {"SSCAN scan_string 0\r\n", wrong_type},
      {"SSCAN scan_ints abc\r\n", "ERR invalid cursor\n"},
      {"SSCAN scan_ints 0 NOVALUES\r\n",
       "ERR NOVALUES option can only be used in HSCAN\n"},
      {"SSCAN scan_ints 0 TYPE set\r\n", "ERR syntax error\n"},
      {"SSCAN scan_ints 0 COUNT x\r\n",
       "ERR value is not an integer or out of range\n"},
  };
  for (const Case& test_case : cases) {
    if (!ExpectReply(&cli, test_case)) {
      return false;
    }
  }
  // Intsets and listpacks are returned whole with cursor 0, in their own
  // order (sorted integers, insertion order), whatever the cursor and COUNT.
  if (!ExpectLines(&cli,
                   {{"SSCAN", "missing_set", "0", "NOVALUES"},
                    {"SSCAN", "scan_ints", "0"},
                    {"SSCAN", "scan_ints", "3", "COUNT", "1"},
                    {"SSCAN", "scan_ints", "0", "MATCH", "*0*"},
                    {"SSCAN", "scan_strings", "0"}},
                   {{"0"},
                    {"0", "-1", "3", "5", "100"},
                    {"0", "-1", "3", "5", "100"},
                    {"0", "100"},
                    {"0", "b", "a", "c", "10"}})) {
    return false;
  }

  // Past 128 members the set is a dict, scanned in steps.
  std::vector<std::string> members;
  for (int i = 0; i < 300; ++i) {
    members.push_back("member:" + std::to_string(i));
    cli.AddCommand(
        std::vector<std::string_view>{"SADD", "scan_big_set", members.back()});
    cli.ReadReply();
  }
  std::ranges::sort(members);
  std::vector<std::string> scanned =
      ScanAll(&cli, "SSCAN", "scan_big_set", {"COUNT", "5"});
  std::ranges::sort(scanned);
  if (scanned != members) {
    RS_LOG_DEBUG("SSCAN did not return every member exactly once\n");
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

  const std::vector<Case> setup_cases = {
      {"SADD integration_set ele1\r\n", "1\n"},
      {"SADD integration_set ele2\r\n", "1\n"},
      {"SADD integration_set ele3\r\n", "1\n"},
      {"SADD integration_set ele4\r\n", "1\n"},
      {"SADD integration_set ele5\r\n", "1\n"},
      {"SADD integration_set ele6\r\n", "1\n"},
      {"SADD integration_set ele7\r\n", "1\n"},
      {"SCARD integration_set\r\n", "7\n"},
      {"SISMEMBER integration_set ele7\r\n", "1\n"},
      {"SISMEMBER integration_set ele9\r\n", "0\n"},
      {"SCARD missing_set\r\n", "0\n"},
      {"SISMEMBER missing_set ele1\r\n", "0\n"},
      {"SREM missing_set ele1 ele2\r\n", "0\n"},
      {"SADD emptied_set only\r\n", "1\n"},
      {"SREM emptied_set only\r\n", "1\n"},
      {"EXISTS emptied_set\r\n", "0\n"},
      {"SADD integer_text_set 1 +1 0 -0\r\n", "4\n"},
      {"SCARD integer_text_set\r\n", "4\n"},
      {"SISMEMBER integer_text_set +1\r\n", "1\n"},
      {"SISMEMBER integer_text_set -0\r\n", "1\n"},
      {"SET set_wrong_type value\r\n", "OK\n"},
      {"SCARD set_wrong_type\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
  };
  for (const Case& test_case : setup_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  if (!ExpectMembers(
          &cli, "SMEMBERS integration_set\r\n",
          {"ele1", "ele2", "ele3", "ele4", "ele5", "ele6", "ele7"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(&cli, "SMEMBERS missing_set\r\n", {})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(&cli, "SMEMBERS integer_text_set\r\n",
                     {"1", "+1", "0", "-0"})) {
    return EXIT_FAILURE;
  }
  const std::vector<Case> operation_setup = {
      {"SADD integration_set_b ele2 ele3 other\r\n", "3\n"},
      {"SADD integration_set_c ele3 ele4\r\n", "2\n"},
  };
  for (const Case& test_case : operation_setup) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }
  if (!ExpectMembers(&cli, "SINTER integration_set integration_set_b\r\n",
                     {"ele2", "ele3"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(
          &cli, "SUNION integration_set\r\n",
          {"ele1", "ele2", "ele3", "ele4", "ele5", "ele6", "ele7"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(
          &cli,
          "SINTER integration_set integration_set_b integration_set_c\r\n",
          {"ele3"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(
          &cli, "SUNION integration_set integration_set_c\r\n",
          {"ele1", "ele2", "ele3", "ele4", "ele5", "ele6", "ele7"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(
          &cli, "SUNION integration_set integration_set_b missing_set\r\n",
          {"ele1", "ele2", "ele3", "ele4", "ele5", "ele6", "ele7", "other"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(&cli, "SDIFF integration_set integration_set_b\r\n",
                     {"ele1", "ele4", "ele5", "ele6", "ele7"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(
          &cli, "SDIFF integration_set integration_set_b integration_set_c\r\n",
          {"ele1", "ele5", "ele6", "ele7"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(&cli, "SINTER integration_set missing_set\r\n", {})) {
    return EXIT_FAILURE;
  }
  // A missing key does not hide a wrong-type key after it.
  for (const std::string_view command :
       {"SUNION integration_set set_wrong_type\r\n",
        "SINTER missing_set set_wrong_type\r\n",
        "SINTER integration_set missing_set set_wrong_type\r\n",
        "SDIFF missing_set set_wrong_type\r\n"}) {
    if (!ExpectReply(&cli, {std::string(command),
                            "WRONGTYPE Operation against a key holding the "
                            "wrong kind of value\n"})) {
      return EXIT_FAILURE;
    }
  }

  const std::vector<Case> remove_cases = {
      {"SREM integration_set ele5 ele6 ele7\r\n", "3\n"},
      {"SREM integration_set ele1 ele6 ele7\r\n", "1\n"},
      {"SCARD integration_set\r\n", "3\n"},
      {"SISMEMBER integration_set ele7\r\n", "0\n"},
      {"SISMEMBER integration_set ele9\r\n", "0\n"},
  };
  for (const Case& test_case : remove_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }
  if (!ExpectMembers(&cli, "SMEMBERS integration_set\r\n",
                     {"ele2", "ele3", "ele4"})) {
    return EXIT_FAILURE;
  }

  if (!ExpectSScan()) {
    return EXIT_FAILURE;
  }

  cli.AddCommand(std::vector<std::string_view>{"HELLO", "3"});
  if (!cli.ReadReply().contains("proto\n3\n")) {
    RS_LOG_DEBUG("failed to negotiate RESP3\n");
    return EXIT_FAILURE;
  }
  return ExpectMembers(&cli, "SMEMBERS integration_set\r\n",
                       {"ele2", "ele3", "ele4"}) &&
                 ExpectMembers(&cli,
                               "SINTER integration_set integration_set_b\r\n",
                               {"ele2", "ele3"})
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
}  // namespace redis_simple

int main() {
  try {
    return redis_simple::Run();
  } catch (...) {
    return EXIT_FAILURE;
  }
}
