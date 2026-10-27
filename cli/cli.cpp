#include "cli/cli.h"

#include <arpa/inet.h>
#include <sys/types.h>

#include <array>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/resp_parser.h"
#include "logging/logger.h"
#include "server/reply.h"
#include "tcp/tcp.h"

namespace redis_simple::cli {
namespace {
bool WriteToConnection(const connection::Connection* connection,
                       std::string_view commands) {
  RS_LOG_DEBUG("client write %.*s\n", static_cast<int>(commands.size()),
               commands.data());
  const ssize_t written =
      connection->SyncWrite(commands.data(), commands.size(), 1000);
  return written >= 0 && static_cast<size_t>(written) == commands.size();
}

std::string ReplyListToString(const std::vector<std::string>& reply) {
  std::string reply_str;
  for (const std::string& r : reply) {
    reply_str.append(r).push_back('\n');
  }
  return reply_str;
}

std::string EncodeCommand(const std::vector<std::string_view>& args) {
  std::string encoded = reply::FromArrayHeader(args.size());
  for (const auto arg : args) {
    reply::AppendBulkString(arg, &encoded);
  }
  return encoded;
}
}  // namespace

RedisCli::RedisCli() : ip_(std::nullopt), port_(std::nullopt) {}

RedisCli::RedisCli(const std::string& ip, int port) : ip_(ip), port_(port) {}

CliStatus RedisCli::Connect(const std::string& ip, int port) {
  connection::Context ctx;
  ctx.loop = nullptr;
  ctx.fd = -1;
  connection_ = std::make_unique<connection::Connection>(ctx);
  const connection::AddressInfo remote(ip, port);
  std::optional<connection::AddressInfo> local;
  if (ip_.has_value() && port_.has_value()) {
    local.emplace(connection::AddressInfo(*ip_, *port_));
  }
  connection::ConnectionStatus st =
      connection_->BindAndBlockingConnect(remote, local, 1000);
  return st == connection::ConnectionStatus::kError ? CliStatus::kError
                                                    : CliStatus::kOk;
}

void RedisCli::AddCommand(const std::string& cmd) {
  const std::scoped_lock lock(lock_);
  query_buf_.Append(cmd.c_str(), cmd.size());
}

void RedisCli::AddCommand(const std::vector<std::string_view>& args) {
  const std::scoped_lock lock(lock_);
  const std::string encoded = EncodeCommand(args);
  query_buf_.Append(encoded.data(), encoded.size());
}

std::string RedisCli::ReadReply() {
  const std::scoped_lock lock(lock_);
  // Drain any buffered complete reply before touching the socket.
  const auto result = MaybeReadReply();
  return result.has_value() ? *result : ReadReplyFromConnection();
}

std::future<std::string> RedisCli::ReadReplyAsync() {
  return std::async(std::launch::async, [this]() { return ReadReply(); });
}

std::optional<std::string> RedisCli::MaybeReadReply() {
  std::vector<std::string> reply;
  if (reply_buf_.Empty()) {
    return std::nullopt;
  }
  const auto status = ProcessReply(reply);
  if (status == resp_parser::ParseStatus::kComplete) {
    return ReplyListToString(reply);
  }
  if (status == resp_parser::ParseStatus::kInvalid) {
    reply_buf_.Clear();
    return kErrResp;
  }
  return std::nullopt;
}

std::string RedisCli::ReadReplyFromConnection() {
  std::vector<std::string> reply;
  if (!query_buf_.Empty()) {
    if (!WriteToConnection(connection_.get(), query_buf_.View())) {
      return kErrResp;
    }
    query_buf_.Clear();
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t read =
        connection_->SyncRead(buffer.data(), buffer.size(), 1000);
    if (read <= 0) {
      return kNoReplyResp;
    }
    reply_buf_.Append(buffer.data(), static_cast<size_t>(read));
    const auto status = ProcessReply(reply);
    if (status == resp_parser::ParseStatus::kComplete) {
      return ReplyListToString(reply);
    }
    if (status == resp_parser::ParseStatus::kInvalid) {
      reply_buf_.Clear();
      return kErrResp;
    }
  }
}

resp_parser::ParseStatus RedisCli::ProcessReply(
    std::vector<std::string>& reply) {
  const auto result = resp_parser::Parse(reply_buf_.View(), reply);
  if (result.status == resp_parser::ParseStatus::kComplete) {
    reply_buf_.Consume(result.consumed);
    reply_buf_.Compact();
  }
  return result.status;
}
}  // namespace redis_simple::cli
