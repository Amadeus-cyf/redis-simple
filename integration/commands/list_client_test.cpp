#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "cli/cli.h"
#include "integration/client_helpers.h"
#include "logging/logger.h"

namespace redis_simple {
namespace {
using integration::Case;
using integration::ExpectReply;
using integration::NonEmptyLines;

bool ExpectLines(cli::RedisCli* cli, const std::string& command,
                 const std::vector<std::string>& expected_lines) {
  cli->AddCommand(command);
  const std::vector<std::string> actual_lines = NonEmptyLines(cli->ReadReply());
  if (actual_lines != expected_lines) {
    RS_LOG_DEBUG("line command failed: %s\n", command.c_str());
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
      {"RPUSH integration_list one two three\r\n", "3\n"},
      {"LLEN integration_list\r\n", "3\n"},
      {"LINDEX integration_list 1\r\n", "two\n"},
      {"LINDEX integration_list -1\r\n", "three\n"},
      {"LINDEX integration_list 10\r\n", "(nil)\n"},
      {"LSET integration_list 1 changed\r\n", "OK\n"},
      {"LINDEX integration_list 1\r\n", "changed\n"},
      {"LPUSH integration_list zero\r\n", "4\n"},
      {"LPOP integration_list\r\n", "zero\n"},
      {"RPOP integration_list\r\n", "three\n"},
      {"LLEN integration_list\r\n", "2\n"},
  };
  for (const Case& test_case : setup_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  if (!ExpectLines(&cli, "LRANGE integration_list 0 -1\r\n",
                   {"one", "changed"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectLines(&cli, "LRANGE integration_list -2 -1\r\n",
                   {"one", "changed"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectLines(&cli, "LRANGE integration_list 10 20\r\n", {})) {
    return EXIT_FAILURE;
  }

  const std::vector<Case> mutation_cases = {
      {"RPUSH integration_list two three two\r\n", "5\n"},
      {"LREM integration_list 1 two\r\n", "1\n"},
      {"LREM integration_list -1 two\r\n", "1\n"},
      {"LTRIM integration_list 1 2\r\n", "OK\n"},
  };
  for (const Case& test_case : mutation_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }
  if (!ExpectLines(&cli, "LRANGE integration_list 0 -1\r\n",
                   {"changed", "three"})) {
    return EXIT_FAILURE;
  }

  const std::vector<Case> missing_and_type_cases = {
      {"LPOP integration_list\r\n", "changed\n"},
      {"LPOP integration_list\r\n", "three\n"},
      {"LPOP integration_list\r\n", "(nil)\n"},
      {"LLEN integration_list\r\n", "0\n"},
      {"RPUSH integration_list recreated\r\n", "1\n"},
      {"LPOP integration_list\r\n", "recreated\n"},
      {"SET list_string value PX 1000\r\n", "OK\n"},
      {"LLEN list_string\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
      {"LPUSH list_string value\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
      {"LRANGE list_string 0 -1\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
  };
  for (const Case& test_case : missing_and_type_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  // LPOP and RPOP with a count, and Redis' order of checks.
  const std::vector<Case> count_cases = {
      {"RPUSH list_count a b c d\r\n", "4\n"},
      {"LPOP list_count -1\r\n",
       "ERR value is out of range, must be positive\n"},
      {"LPOP list_count 1 2\r\n",
       "ERR wrong number of arguments for 'lpop' command\n"},
      {"LPOP missing_list 2\r\n", "(nil)\n"},
      {"LINDEX missing_list notanumber\r\n", "(nil)\n"},
      {"LSET missing_list notanumber value\r\n", "ERR no such key\n"},
      {"LINDEX list_string notanumber\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
  };
  for (const Case& test_case : count_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }
  if (!ExpectLines(&cli, "LPOP list_count 0\r\n", {}) ||
      !ExpectLines(&cli, "LPOP list_count 2\r\n", {"a", "b"}) ||
      !ExpectLines(&cli, "RPOP list_count 5\r\n", {"d", "c"}) ||
      !ExpectReply(&cli, {"EXISTS list_count\r\n", "0\n"})) {
    return EXIT_FAILURE;
  }

  return ExpectLines(&cli, "LRANGE missing_list 0 -1\r\n", {}) ? EXIT_SUCCESS
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
