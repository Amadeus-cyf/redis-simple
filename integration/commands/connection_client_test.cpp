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
  RS_LOG_DEBUG("connection command failed: expected: %.*s actual: %s\n",
               static_cast<int>(expected.size()), expected.data(),
               actual.c_str());
  return false;
}
}  // namespace

int Run() {
  cli::RedisCli cli;
  if (cli.Connect("localhost", 8080) == cli::CliStatus::kError) {
    RS_LOG_DEBUG("failed to connect to integration server\n");
    return EXIT_FAILURE;
  }

  cli.AddCommand(std::vector<std::string_view>{"HELLO", "3"});
  if (cli.ReadReply().find("proto\n3\n") == std::string::npos) {
    RS_LOG_DEBUG("failed to negotiate RESP3\n");
    return EXIT_FAILURE;
  }

  const std::string binary_message("hello\0world", 11);
  std::string binary_reply = binary_message;
  binary_reply.push_back('\n');
  if (!ExpectReply(&cli, {"PING"}, "PONG\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"PING", "hello world"}, "hello world\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"ECHO", binary_message}, binary_reply)) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"PING", "too", "many"},
                   "ERR wrong number of arguments\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"AUTH", "secret"},
                   "ERR AUTH <password> called without any password "
                   "configured for the default user. Are you sure your "
                   "configuration is correct?\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"ECHO"}, "ERR wrong number of arguments\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"QUIT", "now"}, "ERR wrong number of arguments\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"BGREWRITEAOF"},
                   "ERR append only file is disabled\n")) {
    return EXIT_FAILURE;
  }
  cli.AddCommand(std::vector<std::string_view>{"INFO", "persistence"});
  const std::string persistence_info = cli.ReadReply();
  if (persistence_info.find("aof_enabled:0") == std::string::npos ||
      persistence_info.find("aof_last_error:none") == std::string::npos) {
    RS_LOG_DEBUG("unexpected persistence info: %s\n", persistence_info.c_str());
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"QUIT"}, "OK\n")) {
    return EXIT_FAILURE;
  }

  return cli.ReadReply() == "no_reply" ? EXIT_SUCCESS : EXIT_FAILURE;
}
}  // namespace redis_simple

int main() {
  try {
    return redis_simple::Run();
  } catch (...) {
    return EXIT_FAILURE;
  }
}
