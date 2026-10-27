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
#include "memory/dynamic_buffer.h"

namespace redis_simple::cli {
enum class CliStatus {
  kOk = 1,
  kError = -1,
};

class RedisCli {
 public:
  RedisCli();
  RedisCli(const std::string& ip, int port);
  CliStatus Connect(const std::string& ip, int port);
  void AddCommand(const std::string& cmd);
  void AddCommand(const std::vector<std::string_view>& args);
  std::string ReadReply();
  std::future<std::string> ReadReplyAsync();
  ~RedisCli() = default;

 private:
  static constexpr const char* kErrResp = "error";
  static constexpr const char* kNoReplyResp = "no_reply";
  std::optional<std::string> MaybeReadReply();
  std::string ReadReplyFromConnection();
  resp_parser::ParseStatus ProcessReply(std::vector<std::string>& reply);
  std::unique_ptr<connection::Connection> connection_;
  std::optional<std::string> ip_;
  std::optional<int> port_;
  in_memory::DynamicBuffer query_buf_;
  in_memory::DynamicBuffer reply_buf_;
  std::mutex lock_;
};
}  // namespace redis_simple::cli

#endif  // REDIS_SIMPLE_CLI_CLI_H_
