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
bool ExpectRawReply(cli::RedisCli* cli, const std::string& request,
                    const std::string& expected) {
  cli->AddCommand(request);
  const std::string reply = cli->ReadReply();
  if (reply != expected) {
    RS_LOG_DEBUG("raw request failed: expected %s actual %s\n",
                 expected.c_str(), reply.c_str());
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

  cli.AddCommand(std::vector<std::string_view>{"HELLO", "3"});
  if (!cli.ReadReply().contains("proto\n3\n")) {
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
                   "ERR wrong number of arguments for 'ping' command\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(
          &cli, {"HELLO", "three"},
          "ERR Protocol version is not an integer or out of range\n") ||
      !ExpectReply(&cli, {"HELLO", "4"},
                   "NOPROTO unsupported protocol version\n")) {
    return EXIT_FAILURE;
  }
  // Without a password, the default user accepts any password, as Redis'
  // nopass default user does, and other users do not exist.
  if (!ExpectReply(&cli, {"AUTH", "default", "anything"}, "OK\n") ||
      !ExpectReply(&cli, {"AUTH", "someone", "anything"},
                   "WRONGPASS invalid username-password pair or user is "
                   "disabled.\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"AUTH", "secret"},
                   "ERR AUTH <password> called without any password "
                   "configured for the default user. Are you sure your "
                   "configuration is correct?\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"ECHO"},
                   "ERR wrong number of arguments for 'echo' command\n")) {
    return EXIT_FAILURE;
  }
  // AUTH with too many arguments, HELLO's options, CLIENT, and SELECT, as
  // Redis answers them.
  if (!ExpectReply(&cli, {"AUTH", "a", "b", "c"}, "ERR syntax error\n") ||
      !ExpectReply(&cli, {"HELLO", "3", "BOGUS"},
                   "ERR Syntax error in HELLO option 'BOGUS'\n") ||
      !ExpectReply(&cli, {"HELLO", "3", "AUTH", "someone", "pw"},
                   "WRONGPASS invalid username-password pair or user is "
                   "disabled.\n") ||
      !ExpectReply(&cli, {"HELLO", "3", "SETNAME", "a b"},
                   "ERR Client names cannot contain spaces, newlines or "
                   "special characters.\n")) {
    return EXIT_FAILURE;
  }
  cli.AddCommand(std::vector<std::string_view>{"HELLO", "3", "AUTH", "default",
                                               "pw", "SETNAME", "conn-1"});
  if (!cli.ReadReply().contains("proto\n3\n")) {
    RS_LOG_DEBUG("HELLO with AUTH and SETNAME failed\n");
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"CLIENT", "GETNAME"}, "conn-1\n") ||
      !ExpectReply(&cli, {"CLIENT", "SETNAME", ""}, "OK\n") ||
      !ExpectReply(&cli, {"CLIENT", "GETNAME"}, "(nil)\n") ||
      !ExpectReply(&cli, {"CLIENT", "SETINFO", "lib-name", "redis-simple"},
                   "OK\n") ||
      !ExpectReply(&cli, {"CLIENT", "GETNAME", "extra"},
                   "ERR wrong number of arguments for 'client|getname' "
                   "command\n") ||
      !ExpectReply(&cli, {"CLIENT", "NOPE"},
                   "ERR unknown subcommand 'NOPE'. Try CLIENT HELP.\n") ||
      !ExpectReply(&cli, {"SELECT", "0"}, "OK\n") ||
      !ExpectReply(&cli, {"SELECT", "1"}, "ERR DB index is out of range\n")) {
    return EXIT_FAILURE;
  }
  cli.AddCommand(std::vector<std::string_view>{"CLIENT", "ID"});
  if (cli.ReadReply().empty()) {
    return EXIT_FAILURE;
  }
  // Inline requests are split as Redis splits them: quotes and escapes, and
  // empty lines skipped.
  if (!ExpectRawReply(&cli, "\r\nECHO \"a b\\x41\" 'c'\r\n",
                      "ERR wrong number of arguments for 'echo' command\n") ||
      !ExpectRawReply(&cli, "ECHO \"a b\\x41\"\r\n", "a bA\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"QUIT", "now"},
                   "ERR wrong number of arguments for 'quit' command\n")) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"BGREWRITEAOF"},
                   "ERR append only file is disabled\n")) {
    return EXIT_FAILURE;
  }
  cli.AddCommand(std::vector<std::string_view>{"INFO", "persistence"});
  const std::string persistence_info = cli.ReadReply();
  if (!persistence_info.contains("aof_enabled:0") ||
      !persistence_info.contains("aof_last_write_status:ok")) {
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
