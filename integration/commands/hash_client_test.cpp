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
using integration::ExpectReply;
using integration::NonEmptyLines;
using integration::ReplyLines;
using integration::ScanAll;

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

std::vector<std::pair<std::string, std::string>> SortedPairs(
    const std::vector<std::string>& elements) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (size_t i = 0; i + 1 < elements.size(); i += 2) {
    pairs.emplace_back(elements[i], elements[i + 1]);
  }
  std::ranges::sort(pairs);
  return pairs;
}

bool ExpectHScan() {
  cli::RedisCli cli;
  if (cli.Connect("localhost", 8080) == cli::CliStatus::kError) {
    return false;
  }
  const std::string wrong_type =
      "WRONGTYPE Operation against a key holding the wrong kind of value\n";
  const std::vector<Case> cases = {
      {"HSET scan_hash f1 v1 f2 2 g1 x\r\n", "3\n"},
      {"SET scan_string value\r\n", "OK\n"},
      {"HSCAN scan_string 0\r\n", wrong_type},
      // The cursor is checked first, then the key, then the options.
      {"HSCAN scan_string abc\r\n", "ERR invalid cursor\n"},
      {"HSCAN scan_string 0 BAD\r\n", wrong_type},
      {"HSCAN scan_hash -1\r\n", "ERR invalid cursor\n"},
      {"HSCAN scan_hash 0 BAD\r\n", "ERR syntax error\n"},
      {"HSCAN scan_hash 0 TYPE hash\r\n", "ERR syntax error\n"},
      {"HSCAN scan_hash 0 COUNT 0\r\n", "ERR syntax error\n"},
      {"HSCAN scan_hash 0 MATCH\r\n", "ERR syntax error\n"},
  };
  for (const Case& test_case : cases) {
    if (!ExpectReply(&cli, test_case)) {
      return false;
    }
  }
  // A missing key is an empty, complete scan, whatever the options.
  const std::vector<std::string> empty_scan = {"0"};
  // A listpack hash is returned whole, in insertion order, with cursor 0,
  // whatever the cursor and COUNT, as Redis does.
  const std::vector<std::vector<std::string_view>> requests = {
      {"HSCAN", "missing_hash", "0", "BAD"},
      {"HSCAN", "scan_hash", "0"},
      {"HSCAN", "scan_hash", "7", "COUNT", "1"},
      {"HSCAN", "scan_hash", "0", "MATCH", "f*"},
      {"HSCAN", "scan_hash", "0", "novalues"},
      {"HSCAN", "scan_hash", "0", "NOVALUES", "MATCH", "g*"},
  };
  const std::vector<std::vector<std::string>> expected = {
      empty_scan,
      {"0", "f1", "v1", "f2", "2", "g1", "x"},
      {"0", "f1", "v1", "f2", "2", "g1", "x"},
      {"0", "f1", "v1", "f2", "2"},
      {"0", "f1", "f2", "g1"},
      {"0", "g1"},
  };
  for (size_t i = 0; i < requests.size(); ++i) {
    if (ReplyLines(&cli, requests[i]) != expected[i]) {
      RS_LOG_DEBUG("HSCAN request %zu failed\n", i);
      return false;
    }
  }

  // Past 512 entries the hash is a dict, scanned in steps.
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 600; ++i) {
    entries.emplace_back("field:" + std::to_string(i),
                         "value:" + std::to_string(i));
    cli.AddCommand(std::vector<std::string_view>{
        "HSET", "scan_big_hash", entries.back().first, entries.back().second});
    cli.ReadReply();
  }
  std::ranges::sort(entries);
  if (SortedPairs(ScanAll(&cli, "HSCAN", "scan_big_hash", {"COUNT", "7"})) !=
      entries) {
    RS_LOG_DEBUG("HSCAN did not return every entry exactly once\n");
    return false;
  }
  std::vector<std::string> fields =
      ScanAll(&cli, "HSCAN", "scan_big_hash", {"NOVALUES", "MATCH", "*:5?"});
  std::ranges::sort(fields);
  std::vector<std::string> expected_fields;
  for (int i = 50; i < 60; ++i) {
    expected_fields.push_back("field:" + std::to_string(i));
  }
  std::ranges::sort(expected_fields);
  if (fields != expected_fields) {
    RS_LOG_DEBUG("HSCAN NOVALUES MATCH failed\n");
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
      {"HMSET hmset_hash a 1 b 2\r\n", "OK\n"},
      {"HMSET hmset_hash a 3\r\n", "OK\n"},
      {"HGET hmset_hash a\r\n", "3\n"},
      {"HGET hmset_hash b\r\n", "2\n"},
      {"HMSET hmset_hash a\r\n",
       "ERR wrong number of arguments for 'hmset' command\n"},
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
      {"HINCRBY integration_hash name 1\r\n",
       "ERR hash value is not an integer\n"},
      {"HINCRBY integration_hash counter invalid\r\n",
       "ERR value is not an integer or out of range\n"},
      {"HINCRBY integration_hash counter 9223372036854775807\r\n",
       "ERR increment or decrement would overflow\n"},
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

  if (!ExpectHScan()) {
    return EXIT_FAILURE;
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
