#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cli/cli.h"
#include "logging/logger.h"
#include "utils/string_utils.h"

namespace redis_simple {
namespace {
struct Case {
  std::string command;
  std::string expected_reply;
};

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

bool ExpectIntegerBetween(cli::RedisCli* cli, const std::string& command,
                          int64_t min_value, int64_t max_value) {
  cli->AddCommand(command);
  const std::string reply = cli->ReadReply();
  int64_t value = 0;
  if (reply.empty() ||
      !utils::ToInt64(reply.substr(0, reply.size() - 1), &value) ||
      value < min_value || value > max_value) {
    RS_LOG_DEBUG("command failed: %s expected range: [%" PRId64 ", %" PRId64
                 "] actual: %s\n",
                 command.c_str(), min_value, max_value, reply.c_str());
    return false;
  }
  return true;
}

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

bool ExpectScanKeys(cli::RedisCli* cli, std::string_view pattern,
                    std::vector<std::string> expected_keys) {
  std::vector<std::string> actual_keys;
  std::string cursor = "0";
  size_t calls = 0;
  while (true) {
    cli->AddCommand({"SCAN", cursor, "MATCH", pattern, "COUNT", "1"});
    auto lines = NonEmptyLines(cli->ReadReply());
    if (lines.empty()) {
      RS_LOG_DEBUG("SCAN returned no cursor\n");
      return false;
    }
    cursor = std::move(lines.front());
    actual_keys.insert(actual_keys.end(),
                       std::make_move_iterator(lines.begin() + 1),
                       std::make_move_iterator(lines.end()));
    if (++calls > 1024) {
      RS_LOG_DEBUG("SCAN did not complete\n");
      return false;
    }
    if (cursor == "0") {
      break;
    }
  }

  std::ranges::sort(actual_keys);
  std::ranges::sort(expected_keys);
  return actual_keys == expected_keys;
}
}  // namespace

int Run() {
  cli::RedisCli cli;
  if (cli.Connect("localhost", 8080) == cli::CliStatus::kError) {
    RS_LOG_DEBUG("failed to connect to integration server\n");
    return EXIT_FAILURE;
  }

  const std::vector<Case> setup_cases = {
      {"NOTACOMMAND\r\n", "ERR unknown command 'NOTACOMMAND'\n"},
      {"TYPE missing_key\r\n", "none\n"},
      {"EXISTS missing_key\r\n", "0\n"},
      {"SET key_string value\r\n", "OK\n"},
      {"SADD key_set member\r\n", "1\n"},
      {"RPUSH key_list one two\r\n", "2\n"},
      {"ZADD key_zset 1.0 member\r\n", "1\n"},
      {"HSET key_hash field value\r\n", "1\n"},
      {"TYPE key_string\r\n", "string\n"},
      {"TYPE key_set\r\n", "set\n"},
      {"TYPE key_list\r\n", "list\n"},
      {"TYPE key_zset\r\n", "zset\n"},
      {"TYPE key_hash\r\n", "hash\n"},
      {"EXISTS key_string key_set key_list key_zset key_hash missing_key\r\n",
       "5\n"},
  };

  for (const Case& test_case : setup_cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  if (!ExpectScanKeys(
          &cli, "key_*",
          {"key_hash", "key_list", "key_set", "key_string", "key_zset"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectScanKeys(&cli, "missing:*", {})) {
    return EXIT_FAILURE;
  }

  const std::vector<Case> cases = {
      {"SCAN invalid\r\n", "ERR invalid cursor\n"},
      {"SCAN 0 COUNT 0\r\n", "ERR value is not an integer or out of range\n"},
      {"SCAN 0 UNKNOWN value\r\n", "ERR syntax error\n"},
      {"DEL key_string missing_key\r\n", "1\n"},
      {"TYPE key_string\r\n", "none\n"},
      {"EXISTS key_string key_set\r\n", "1\n"},
      {"DEL key_set key_list key_zset key_hash\r\n", "4\n"},
      {"EXISTS key_set key_list key_zset key_hash\r\n", "0\n"},
      {"TYPE key_set\r\n", "none\n"},
      {"SET expire_key value\r\n", "OK\n"},
      {"EXPIRE expire_key 10\r\n", "1\n"},
      {"PERSIST expire_key\r\n", "1\n"},
      {"TTL expire_key\r\n", "-1\n"},
      {"PEXPIRE expire_key 5\r\n", "1\n"},
      {"SET expireat_key value\r\n", "OK\n"},
      {"EXPIREAT expireat_key 4102444800 NX\r\n", "1\n"},
      {"GET expireat_key\r\n", "value\n"},
      {"SET conditional_expire_key value\r\n", "OK\n"},
      {"EXPIRE conditional_expire_key 60 NX\r\n", "1\n"},
      {"EXPIRE conditional_expire_key 30 NX\r\n", "0\n"},
      {"EXPIRE conditional_expire_key 120 GT\r\n", "1\n"},
      {"PEXPIRE conditional_expire_key 240000 LT\r\n", "0\n"},
      {"PEXPIRE conditional_expire_key 60000 LT\r\n", "1\n"},
      {"PEXPIRE conditional_expire_key 120000 XX\r\n", "1\n"},
      {"PERSIST conditional_expire_key\r\n", "1\n"},
      {"PEXPIRE conditional_expire_key 60000 XX\r\n", "0\n"},
      {"PEXPIRE conditional_expire_key 60000 GT\r\n", "0\n"},
      {"PEXPIRE conditional_expire_key 60000 LT\r\n", "1\n"},
      {"PERSIST conditional_expire_key\r\n", "1\n"},
      {"PEXPIREAT conditional_expire_key 4102444800000 NX\r\n", "1\n"},
      {"PEXPIREAT conditional_expire_key 4102444799000 XX\r\n", "1\n"},
      {"EXPIREAT conditional_expire_key 4102444800 GT\r\n", "1\n"},
      {"PEXPIREAT conditional_expire_key 4102444799000 LT\r\n", "1\n"},
      {"EXPIRE conditional_expire_key invalid\r\n",
       "ERR value is not an integer or out of range\n"},
      {"EXPIRE conditional_expire_key 10 UNKNOWN\r\n", "ERR syntax error\n"},
      {"EXPIRE conditional_expire_key 10 NX XX\r\n", "ERR syntax error\n"},
      {"EXPIREAT conditional_expire_key 9223372036854775807\r\n",
       "ERR invalid expire time\n"},
      {"SET absolute_expire_key value\r\n", "OK\n"},
      {"PEXPIREAT absolute_expire_key 1\r\n", "1\n"},
      {"GET absolute_expire_key\r\n", "(nil)\n"},
      {"PERSIST missing_expire_key\r\n", "0\n"},
      {"EXPIRE missing_expire_key 10\r\n", "0\n"},
      {"PTTL missing_expire_key\r\n", "-2\n"},
      {"SET rename_source value\r\n", "OK\n"},
      {"RENAME rename_source rename_dest\r\n", "OK\n"},
      {"GET rename_dest\r\n", "value\n"},
      {"UNLINK rename_dest missing_key\r\n", "1\n"},
  };

  for (const Case& test_case : cases) {
    if (!ExpectReply(&cli, test_case)) {
      return EXIT_FAILURE;
    }
  }

  if (!ExpectReply(&cli, {"SET ttl_key value PX 5000\r\n", "OK\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectIntegerBetween(&cli, "TTL ttl_key\r\n", 1, 5)) {
    return EXIT_FAILURE;
  }
  if (!ExpectIntegerBetween(&cli, "PTTL ttl_key\r\n", 1, 5000)) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"SET ttl_key kept KEEPTTL\r\n", "OK\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectIntegerBetween(&cli, "PTTL ttl_key\r\n", 1, 5000)) {
    return EXIT_FAILURE;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!ExpectReply(&cli, {"GET expire_key\r\n", "(nil)\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"DBSIZE\r\n", "3\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"FLUSHDB\r\n", "OK\n"})) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"DBSIZE\r\n", "0\n"})) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
}  // namespace redis_simple

int main() {
  try {
    return redis_simple::Run();
  } catch (...) {
    return EXIT_FAILURE;
  }
}
