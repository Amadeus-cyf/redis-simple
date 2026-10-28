#include <algorithm>
#include <cstdlib>
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
  if (!ExpectReply(
          &cli, {"SUNION integration_set set_wrong_type\r\n",
                 "WRONGTYPE Operation against a key holding the wrong kind of "
                 "value\n"})) {
    return EXIT_FAILURE;
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
