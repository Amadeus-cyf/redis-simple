#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "cli/cli.h"
#include "logging/logger.h"

namespace redis_simple {
namespace {
bool ExpectReply(cli::RedisCli* const cli,
                 const std::vector<std::string_view>& command,
                 std::string_view expected) {
  cli->AddCommand(command);
  const std::string actual = cli->ReadReply();
  if (actual == expected) {
    return true;
  }
  RS_LOG_DEBUG("transaction command failed: expected: %.*s actual: %s\n",
               static_cast<int>(expected.size()), expected.data(),
               actual.c_str());
  return false;
}

struct Step {
  cli::RedisCli* cli;
  std::vector<std::string_view> command;
  std::string_view expected;
};

bool RunSteps(const std::vector<Step>& steps) {
  return std::ranges::all_of(steps, [](const Step& step) {
    return ExpectReply(step.cli, step.command, step.expected);
  });
}
}  // namespace

int Run() {
  cli::RedisCli client;
  cli::RedisCli other;
  if (client.Connect("localhost", 8080) == cli::CliStatus::kError ||
      other.Connect("localhost", 8080) == cli::CliStatus::kError) {
    RS_LOG_DEBUG("failed to connect to integration server\n");
    return EXIT_FAILURE;
  }

  cli::RedisCli* const a = &client;
  cli::RedisCli* const b = &other;
  const std::vector<Step> steps = {
      // Queued commands run in order at EXEC; their replies form one array.
      {a, {"MULTI"}, "OK\n"},
      {a, {"SET", "tx_counter", "1"}, "QUEUED\n"},
      {a, {"INCR", "tx_counter"}, "QUEUED\n"},
      {b, {"GET", "tx_counter"}, "(nil)\n"},
      {a, {"GET", "tx_counter"}, "QUEUED\n"},
      {a, {"EXEC"}, "OK\n2\n2\n\n\n"},
      {b, {"GET", "tx_counter"}, "2\n"},
      // Runtime errors are reported in place without stopping the batch.
      {a, {"MULTI"}, "OK\n"},
      {a, {"LPUSH", "tx_counter", "x"}, "QUEUED\n"},
      {a, {"INCR", "tx_counter"}, "QUEUED\n"},
      {a,
       {"EXEC"},
       "WRONGTYPE Operation against a key holding the wrong kind of value\n"
       "3\n\n\n"},
      // Commands rejected while queueing abort the transaction.
      {a, {"MULTI"}, "OK\n"},
      {a, {"INCR", "tx_counter"}, "QUEUED\n"},
      {a, {"GET"}, "ERR wrong number of arguments for 'get' command\n"},
      {a,
       {"EXEC"},
       "EXECABORT Transaction discarded because of previous "
       "errors.\n"},
      {b, {"GET", "tx_counter"}, "3\n"},
      // DISCARD drops the queue.
      {a, {"MULTI"}, "OK\n"},
      {a, {"DEL", "tx_counter"}, "QUEUED\n"},
      {a, {"DISCARD"}, "OK\n"},
      {a, {"EXISTS", "tx_counter"}, "1\n"},
      {a, {"EXEC"}, "ERR EXEC without MULTI\n"},
      {a, {"DISCARD"}, "ERR DISCARD without MULTI\n"},
      // A write by another client to a WATCHed key aborts EXEC.
      {a, {"WATCH", "tx_counter"}, "OK\n"},
      {b, {"INCR", "tx_counter"}, "4\n"},
      {a, {"MULTI"}, "OK\n"},
      {a, {"WATCH", "tx_counter"}, "ERR WATCH inside MULTI is not allowed\n"},
      {a, {"SET", "tx_counter", "0"}, "QUEUED\n"},
      {a, {"EXEC"}, "(nil)\n"},
      {b, {"GET", "tx_counter"}, "4\n"},
      // EXEC unwatched the key, and an untouched WATCH lets EXEC run.
      {a, {"WATCH", "tx_counter", "tx_missing"}, "OK\n"},
      {b, {"DEL", "tx_missing"}, "0\n"},
      {a, {"MULTI"}, "OK\n"},
      {a, {"SET", "tx_counter", "0"}, "QUEUED\n"},
      {a, {"EXEC"}, "OK\n\n\n"},
      // UNWATCH forgets earlier modifications.
      {a, {"WATCH", "tx_counter"}, "OK\n"},
      {b, {"SET", "tx_counter", "5"}, "OK\n"},
      {a, {"UNWATCH"}, "OK\n"},
      {a, {"MULTI"}, "OK\n"},
      {a, {"GET", "tx_counter"}, "QUEUED\n"},
      {a, {"EXEC"}, "5\n\n\n"},
      // Deleting a WATCHed key aborts EXEC too.
      {a, {"WATCH", "tx_counter"}, "OK\n"},
      {b, {"DEL", "tx_counter"}, "1\n"},
      {a, {"MULTI"}, "OK\n"},
      {a, {"PING"}, "QUEUED\n"},
      {a, {"EXEC"}, "(nil)\n"},
      {a, {"MULTI"}, "OK\n"},
      {a, {"EXEC"}, "\n\n"},
  };
  if (!RunSteps(steps)) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(a, {"QUIT"}, "OK\n") || !ExpectReply(b, {"QUIT"}, "OK\n")) {
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
