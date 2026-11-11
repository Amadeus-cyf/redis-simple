#include <algorithm>
#include <cstdint>
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
constexpr int kRdbPort = 18082;

struct Case {
  std::string command;
  std::string expected_reply;
};

std::vector<std::string> NonEmptyLines(const std::string& reply);

// Compares line by line, since the client ends arrays with a blank line.
bool ExpectReply(cli::RedisCli* const cli, const Case& test_case) {
  cli->AddCommand(test_case.command);
  const std::string reply = cli->ReadReply();
  if (NonEmptyLines(reply) == NonEmptyLines(test_case.expected_reply)) {
    return true;
  }
  RS_LOG_DEBUG("RDB command failed: %s expected: %s actual: %s\n",
               test_case.command.c_str(), test_case.expected_reply.c_str(),
               reply.c_str());
  return false;
}

bool ExpectReplies(cli::RedisCli* const cli, const std::vector<Case>& cases) {
  return std::ranges::all_of(cases, [cli](const Case& test_case) {
    return ExpectReply(cli, test_case);
  });
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

// LASTSAVE's reply, as a number.
int64_t LastSave(cli::RedisCli* const cli) {
  cli->AddCommand("LASTSAVE\r\n");
  const std::string reply = cli->ReadReply();
  return std::strtoll(reply.c_str(), nullptr, 10);
}

// Fills every type, in small and large encodings, then saves.
bool WriteDataset(cli::RedisCli* const cli) {
  std::vector<Case> cases = {
      {"FLUSHDB\r\n", "OK\n"},
      {"SET rdb_string value\r\n", "OK\n"},
      {"SET rdb_int 42\r\n", "OK\n"},
      {"SET rdb_ttl live PXAT 4102444800000\r\n", "OK\n"},
      {"RPUSH rdb_list a 1 -5 bbb\r\n", "4\n"},
      {"SADD rdb_ints 3 1 -2\r\n", "3\n"},
      {"SADD rdb_strings b a\r\n", "2\n"},
      {"ZADD rdb_zset 1.5 a 2 b -inf c\r\n", "3\n"},
      {"HSET rdb_hash f1 v1 n 12345\r\n", "2\n"},
  };
  for (int i = 0; i < 300; ++i) {
    const std::string number = std::to_string(i);
    cases.push_back({"RPUSH rdb_big_list element-" + number + "\r\n",
                     std::to_string(i + 1) + "\n"});
    cases.push_back({"SADD rdb_big_set member-" + number + "\r\n", "1\n"});
    std::string zadd = "ZADD rdb_big_zset ";
    zadd.append(number).append(" member-").append(number).append("\r\n");
    cases.push_back({std::move(zadd), "1\n"});
    std::string hset = "HSET rdb_big_hash field-";
    hset.append(number).append(" ").append(70, 'v').append("\r\n");
    cases.push_back({std::move(hset), "1\n"});
  }
  if (!ExpectReplies(cli, cases)) {
    return false;
  }
  const int64_t before = LastSave(cli);
  if (!ExpectReplies(cli, {
                              {"SAVE\r\n", "OK\n"},
                              {"SAVE extra\r\n",
                               "ERR wrong number of arguments for 'save' "
                               "command\n"},
                              // SAVE is not allowed inside MULTI, which then
                              // aborts.
                              {"MULTI\r\n", "OK\n"},
                              {"SET rdb_not_saved v\r\n", "QUEUED\n"},
                              {"SAVE\r\n",
                               "ERR Command not allowed inside a "
                               "transaction\n"},
                              {"EXEC\r\n",
                               "EXECABORT Transaction discarded because of "
                               "previous errors.\n"},
                          })) {
    return false;
  }
  // LASTSAVE reports the start time before the first save, then the save.
  if (LastSave(cli) < before || before <= 0) {
    RS_LOG_DEBUG("LASTSAVE did not advance\n");
    return false;
  }
  // Written after the save, so the restart must not see it.
  return ExpectReply(cli, {"SET rdb_after_save lost\r\n", "OK\n"});
}

bool ReadDataset(cli::RedisCli* const cli) {
  std::vector<Case> cases = {
      {"GET rdb_string\r\n", "value\n"},
      {"GET rdb_int\r\n", "42\n"},
      {"GET rdb_ttl\r\n", "live\n"},
      {"EXISTS rdb_after_save\r\n", "0\n"},
      {"EXISTS rdb_not_saved\r\n", "0\n"},
      {"LRANGE rdb_list 0 -1\r\n", "a\n1\n-5\nbbb\n"},
      {"SISMEMBER rdb_ints -2\r\n", "1\n"},
      {"SCARD rdb_ints\r\n", "3\n"},
      {"SCARD rdb_strings\r\n", "2\n"},
      {"ZRANGE rdb_zset 0 -1 WITHSCORES\r\n", "c\n-inf\na\n1.5\nb\n2\n"},
      {"HGET rdb_hash n\r\n", "12345\n"},
      {"LLEN rdb_big_list\r\n", "300\n"},
      {"LINDEX rdb_big_list -1\r\n", "element-299\n"},
      {"SCARD rdb_big_set\r\n", "300\n"},
      {"ZSCORE rdb_big_zset member-299\r\n", "299\n"},
      {"HLEN rdb_big_hash\r\n", "300\n"},
      {"HGET rdb_big_hash field-7\r\n", std::string(70, 'v') + "\n"},
      {"DBSIZE\r\n", "12\n"},
  };
  if (!ExpectReplies(cli, cases)) {
    return false;
  }
  cli->AddCommand("PTTL rdb_ttl\r\n");
  const std::vector<std::string> ttl = NonEmptyLines(cli->ReadReply());
  return ttl.size() == 1 && std::strtoll(ttl[0].c_str(), nullptr, 10) > 0;
}

bool ExpectEmpty(cli::RedisCli* const cli) {
  return ExpectReply(cli, {"DBSIZE\r\n", "0\n"});
}

int Run(std::string_view mode) {
  cli::RedisCli cli;
  if (cli.Connect("127.0.0.1", kRdbPort) == cli::CliStatus::kError) {
    return EXIT_FAILURE;
  }
  if (mode == "write") {
    return WriteDataset(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "read") {
    return ReadDataset(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "empty") {
    return ExpectEmpty(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return EXIT_FAILURE;
}
}  // namespace
}  // namespace redis_simple

int main(int argc, char* argv[]) {
  try {
    return argc == 2 ? redis_simple::Run(argv[1]) : EXIT_FAILURE;
  } catch (...) {
    return EXIT_FAILURE;
  }
}
