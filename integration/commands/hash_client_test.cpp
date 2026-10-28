#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

bool ExpectPairs(
    cli::RedisCli* cli, const std::string& command,
    std::vector<std::pair<std::string, std::string>> expected_pairs) {
  cli->AddCommand(command);
  const std::vector<std::string> lines = NonEmptyLines(cli->ReadReply());
  if (lines.size() % 2 != 0) {
    RS_LOG_DEBUG("hash pair command returned odd values: %s\n",
                 command.c_str());
    return false;
  }

  std::vector<std::pair<std::string, std::string>> actual_pairs;
  actual_pairs.reserve(lines.size() / 2);
  for (size_t i = 0; i < lines.size(); i += 2) {
    actual_pairs.emplace_back(lines[i], lines[i + 1]);
  }
  std::ranges::sort(actual_pairs);
  std::ranges::sort(expected_pairs);
  if (actual_pairs != expected_pairs) {
    RS_LOG_DEBUG("hash pair command failed: %s\n", command.c_str());
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
    RS_LOG_DEBUG("hash member command failed: %s\n", command.c_str());
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
      {"HSET integration_hash name redis version 7\r\n", "2\n"},
      {"HSET integration_hash version 8 mode simple\r\n", "1\n"},
      {"HGET integration_hash name\r\n", "redis\n"},
      {"HGET integration_hash version\r\n", "8\n"},
      {"HGET integration_hash missing\r\n", "(nil)\n"},
      {"HGET missing_hash field\r\n", "(nil)\n"},
      {"HEXISTS integration_hash name\r\n", "1\n"},
      {"HEXISTS integration_hash missing\r\n", "0\n"},
      {"HEXISTS missing_hash field\r\n", "0\n"},
      {"HLEN integration_hash\r\n", "3\n"},
      {"HLEN missing_hash\r\n", "0\n"},
      {"HDEL missing_hash field\r\n", "0\n"},
      {"HMGET integration_hash name missing version\r\n",
       "redis\n(nil)\n8\n\n\n"},
      {"HINCRBY integration_hash counter 2\r\n", "2\n"},
      {"HINCRBY integration_hash counter -1\r\n", "1\n"},
      {"HGET integration_hash counter\r\n", "1\n"},
      {"SET hash_wrong_type value\r\n", "OK\n"},
      {"HSET hash_wrong_type field value\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
      {"HGET hash_wrong_type field\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
      {"HLEN hash_wrong_type\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
      {"HGETALL hash_wrong_type\r\n",
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"},
  };
  for (const Case& test_case : cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  if (!ExpectPairs(&cli, "HGETALL integration_hash\r\n",
                   {{"name", "redis"},
                    {"version", "8"},
                    {"mode", "simple"},
                    {"counter", "1"}})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(&cli, "HKEYS integration_hash\r\n",
                     {"name", "version", "mode", "counter"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectMembers(&cli, "HVALS integration_hash\r\n",
                     {"redis", "8", "simple", "1"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectPairs(&cli, "HGETALL missing_hash\r\n", {})) {
    return EXIT_FAILURE;
  }

  const std::string long_value(65, 'v');
  if (!ExpectReply(
          &cli,
          {"HSET integration_hash_long field " + long_value + "\r\n", "1\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(
          &cli, {"HGET integration_hash_long field\r\n", long_value + "\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectPairs(&cli, "HGETALL integration_hash_long\r\n",
                   {{"field", long_value}})) {
    return EXIT_FAILURE;
  }

  const std::string fragmented_value(8192, 'f');
  if (!ExpectReply(&cli, {"HSET integration_hash_fragmented field " +
                              fragmented_value + "\r\n",
                          "1\n"}) ||
      !ExpectReply(&cli, {"HGET integration_hash_fragmented field\r\n",
                          fragmented_value + "\n"})) {
    return EXIT_FAILURE;
  }

  const std::vector<Case> delete_cases = {
      {"HDEL integration_hash name missing\r\n", "1\n"},
      {"HLEN integration_hash\r\n", "3\n"},
      {"HDEL integration_hash version mode counter\r\n", "3\n"},
      {"HLEN integration_hash\r\n", "0\n"},
      {"TYPE integration_hash\r\n", "none\n"},
      {"HSET integration_hash recreated value\r\n", "1\n"},
      {"HGET integration_hash recreated\r\n", "value\n"},
  };
  for (const Case& test_case : delete_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  cli.AddCommand(std::vector<std::string_view>{"HELLO", "3"});
  if (!cli.ReadReply().contains("proto\n3\n")) {
    RS_LOG_DEBUG("failed to negotiate RESP3\n");
    return EXIT_FAILURE;
  }
  return ExpectPairs(&cli, "HGETALL integration_hash\r\n",
                     {{"recreated", "value"}})
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
