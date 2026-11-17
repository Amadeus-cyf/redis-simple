#ifndef REDIS_SIMPLE_CLI_CLI_H_
#define REDIS_SIMPLE_CLI_CLI_H_

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/resp_parser.h"
#include "connection/connection.h"
#include "containers/dynamic_buffer.h"

namespace redis_simple::cli {
enum class CliStatus {
  kOk = 1,
  kError = -1,
};

// A blocking Redis client for tests and tools. Commands queue in memory
// and are sent by the next ReadReply() that finds no buffered reply. A
// mutex guards the buffers, so ReadReplyAsync() may run beside AddCommand().
class RedisCli {
 public:
  RedisCli();
  // `ip` and `port` are the local address the socket binds to before it
  // connects, not the server's.
  RedisCli(const std::string& ip, int port);
  CliStatus Connect(const std::string& ip, int port);
  // Queues `command`, which must already be encoded as the server expects
  // it (RESP, or an inline command ending in "\r\n").
  void AddCommand(const std::string& command);
  // Queues `args` encoded as a RESP array of bulk strings.
  void AddCommand(const std::vector<std::string_view>& args);
  // Returns one reply with each value on a line of its own, "error" when
  // the queued commands cannot be sent or the reply is not valid RESP, and
  // "no_reply" when the server closes the connection or sends nothing
  // within a second.
  std::string ReadReply();
  std::future<std::string> ReadReplyAsync();
  ~RedisCli() = default;

 private:
  static constexpr std::string_view kErrorResponse = "error";
  static constexpr std::string_view kNoReplyResponse = "no_reply";
  std::optional<std::string> MaybeReadReply();
  std::string ReadReplyFromConnection();
  resp_parser::ParseStatus ProcessReply(std::vector<std::string>& reply);
  std::unique_ptr<connection::Connection> connection_;
  std::optional<std::string> ip_;
  std::optional<int> port_;
  containers::DynamicBuffer query_buffer_;
  containers::DynamicBuffer reply_buffer_;
  std::mutex lock_;
};
}  // namespace redis_simple::cli

#endif  // REDIS_SIMPLE_CLI_CLI_H_
