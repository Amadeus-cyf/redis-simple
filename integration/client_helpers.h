#ifndef REDIS_SIMPLE_INTEGRATION_CLIENT_HELPERS_H_
#define REDIS_SIMPLE_INTEGRATION_CLIENT_HELPERS_H_

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/cli.h"
#include "logging/logger.h"

// Helpers the command integration tests share. Each test is its own
// executable, so these are inline.
namespace redis_simple::integration {
// A request as the client sends it, and the reply text it must produce.
struct Case {
  std::string command;
  std::string expected_reply;
};

// The lines of a rendered reply, without the blank ones.
inline std::vector<std::string> NonEmptyLines(const std::string& reply) {
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

// Sends the case\'s command and compares the reply with what it expects.
// A mismatch is logged as an error, so a failing test says which command
// failed without a debug log level.
inline bool ExpectReply(cli::RedisCli* cli, const Case& test_case) {
  cli->AddCommand(test_case.command);
  const std::string reply = cli->ReadReply();
  if (reply != test_case.expected_reply) {
    RS_LOG_ERROR("command failed: %s expected: %s actual: %s\n",
                 test_case.command.c_str(), test_case.expected_reply.c_str(),
                 reply.c_str());
    return false;
  }
  return true;
}

// Sends `command` and returns the non-empty lines of its reply.
inline std::vector<std::string> ReplyLines(
    cli::RedisCli* cli, const std::vector<std::string_view>& command) {
  cli->AddCommand(command);
  return NonEmptyLines(cli->ReadReply());
}

// Runs `command key cursor options...` from cursor 0 until the cursor
// returns to 0, and returns every element the steps returned.
inline std::vector<std::string> ScanAll(
    cli::RedisCli* cli, std::string_view command, std::string_view key,
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
inline bool ExpectLines(
    cli::RedisCli* cli,
    const std::vector<std::vector<std::string_view>>& requests,
    const std::vector<std::vector<std::string>>& expected) {
  for (size_t i = 0; i < requests.size(); ++i) {
    if (ReplyLines(cli, requests[i]) != expected[i]) {
      RS_LOG_ERROR("request %zu failed\n", i);
      return false;
    }
  }
  return true;
}
}  // namespace redis_simple::integration

#endif  // REDIS_SIMPLE_INTEGRATION_CLIENT_HELPERS_H_
