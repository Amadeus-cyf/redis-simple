#include "client.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "server/aof.h"
#include "server/reply.h"
#include "server/request_parser.h"
#include "utils/time_utils.h"

namespace redis_simple {
namespace {
constexpr auto kReadBufferSize = size_t{16} * 1024;
// Upper bound for one read while receiving a large bulk argument.
constexpr auto kMaxBulkReadSize = size_t{1024} * 1024;
constexpr auto kMaxQueryBufferSize = size_t{64} * 1024 * 1024;

// Pins the database clock for one command.
class CommandTimeScope {
 public:
  explicit CommandTimeScope(db::RedisDb* const db) : db_(db) {
    db_->SetCommandTime(utils::NowInMilliseconds());
  }
  CommandTimeScope(const CommandTimeScope&) = delete;
  CommandTimeScope& operator=(const CommandTimeScope&) = delete;
  ~CommandTimeScope() { db_->ClearCommandTime(); }

 private:
  db::RedisDb* const db_;
};
}  // namespace

Client::Client(std::unique_ptr<connection::Connection> connection,
               db::RedisDb* const db, aof::Aof* const aof,
               OutputBufferLimits output_limits)
    : connection_(std::move(connection)),
      db_(db),
      aof_(aof),
      output_limits_(output_limits) {}

Client::Client(db::RedisDb* const db) : db_(db), discard_replies_(true) {}

size_t Client::AddReply(std::string_view reply_text) {
  if (discard_replies_) {
    reply_error_ =
        reply_error_ || (!reply_text.empty() && reply_text.front() == '-');
    return reply_text.size();
  }
  if (!CanQueueReply(reply_text.size())) {
    return 0;
  }
  return reply_buf_.Append(reply_text.data(), reply_text.size());
}

size_t Client::AddReply(std::string&& reply_text) {
  if (discard_replies_) {
    reply_error_ =
        reply_error_ || (!reply_text.empty() && reply_text.front() == '-');
    return reply_text.size();
  }
  if (!CanQueueReply(reply_text.size())) {
    return 0;
  }
  return reply_buf_.Append(std::move(reply_text));
}

size_t Client::AddReply(std::string&& header, std::string&& body) {
  if (body.size() > std::numeric_limits<size_t>::max() - header.size()) {
    close_after_reply_ = true;
    return 0;
  }
  const size_t total_size = header.size() + body.size();
  if (discard_replies_) {
    return total_size;
  }
  if (!CanQueueReply(total_size)) {
    return 0;
  }
  const size_t header_size = reply_buf_.Append(std::move(header));
  const size_t body_size = reply_buf_.Append(std::move(body));
  return header_size + body_size;
}

size_t Client::AddBulkReply(std::string_view value) {
  constexpr std::string_view kCrlf = "\r\n";
  std::array<char, reply::kBulkStringHeaderCapacity> header{};
  const size_t header_size =
      reply::EncodeBulkStringHeader(value.size(), header.data(), header.size());
  const size_t total_size = header_size + value.size() + kCrlf.size();
  if (discard_replies_) {
    return total_size;
  }
  if (!CanQueueReply(total_size)) {
    return 0;
  }
  reply_buf_.Append(header.data(), header_size);
  reply_buf_.Append(value.data(), value.size());
  reply_buf_.Append(kCrlf.data(), kCrlf.size());
  return total_size;
}

bool Client::ShouldPauseReads() const {
  return !reads_paused_ && (close_after_reply_ ||
                            (output_limits_.soft_bytes > 0 &&
                             PendingReplyBytes() >= output_limits_.soft_bytes));
}

bool Client::ShouldResumeReads() const {
  return reads_paused_ && !close_after_reply_ &&
         PendingReplyBytes() <= output_limits_.resume_bytes;
}

bool Client::CanQueueReply(size_t size) {
  const size_t pending = PendingReplyBytes();
  if (size <= output_limits_.hard_bytes &&
      pending <= output_limits_.hard_bytes - size) {
    return true;
  }
  RS_LOG_DEBUG("client output buffer limit exceeded\n");
  close_after_reply_ = true;
  return false;
}

ssize_t Client::ReadQuery() {
  // One read per readable event: level-triggered polling reports any bytes
  // left in the socket again, so a busy client cannot monopolize the loop.
  // Large bulk arguments are read in bigger steps to save syscalls.
  const size_t pending_bulk =
      parse_state_.PendingBulkBytes(query_buf_.View().size());
  const size_t read_size =
      std::clamp(pending_bulk, kReadBufferSize, kMaxBulkReadSize);
  char* const tail = query_buf_.WritableTail(read_size);
  const ssize_t nread = connection_->Read(tail, read_size);
  if (nread == 0) {
    return 0;
  }
  if (nread < 0) {
    // Would-block leaves the connection usable; other errors mark it failed.
    return connection_->State() == connection::ConnectionState::kConnected ? 0
                                                                           : -1;
  }
  query_buf_.Commit(static_cast<size_t>(nread));
  if (query_buf_.View().size() > kMaxQueryBufferSize) {
    RS_LOG_DEBUG("query buffer limit exceeded\n");
    connection_->SetState(connection::ConnectionState::kError);
    return -1;
  }
  return nread;
}

ssize_t Client::SendReply() {
  return reply_buf_.ReplyCount() > 0 ? SendListReply() : SendBufferReply();
}

ssize_t Client::SendBufferReply() {
  const size_t reply_length = reply_buf_.UnsentLength();
  const int logged_length = static_cast<int>(std::min(
      reply_length, static_cast<size_t>(std::numeric_limits<int>::max())));
  RS_LOG_DEBUG("_sendReply %.*s %zu\n", logged_length,
               reply_buf_.UnsentBuffer(), reply_length);
  ssize_t nwritten =
      connection_->Write(reply_buf_.UnsentBuffer(), reply_buf_.UnsentLength());
  if (nwritten < 0) {
    return -1;
  }
  reply_buf_.Consume(nwritten);
  return nwritten;
}

ssize_t Client::SendListReply() {
  RS_LOG_DEBUG("_sendvReply\n");
  reply_buf_.FillBlocks(&reply_blocks_);
  ssize_t nwritten = connection_->WriteVector(reply_blocks_);
  if (nwritten < 0) {
    return -1;
  }
  reply_buf_.Consume(nwritten);
  return nwritten;
}

ClientStatus Client::ProcessInputBuffer() {
  while (!close_after_reply_ && query_buf_.Consumed() < query_buf_.Size()) {
    RS_LOG_DEBUG("process loop %zu %zu\n", query_buf_.Consumed(),
                 query_buf_.Size());
    const RequestStatus status = ParseRequest();
    if (status == RequestStatus::kIncomplete ||
        status == RequestStatus::kProtocolError) {
      break;
    }
    if (status == RequestStatus::kRejected) {
      continue;
    }
    if (ProcessCommand() == ClientStatus::kError) {
      return ClientStatus::kError;
    }
  }
  query_buf_.Compact();
  return ClientStatus::kOk;
}

Client::RequestStatus Client::ParseRequest() {
  command_ = nullptr;
  args_.clear();
  std::string_view name;
  const auto parsed =
      request_parser::Parse(query_buf_.View(), &parse_state_, &name, &args_);
  if (parsed.status == request_parser::ParseStatus::kIncomplete) {
    return RequestStatus::kIncomplete;
  }
  if (parsed.status == request_parser::ParseStatus::kInvalid) {
    AddReply(reply::FromError("ERR Protocol error: invalid request"));
    close_after_reply_ = true;
    query_buf_.Clear();
    return RequestStatus::kProtocolError;
  }
  query_buf_.Consume(parsed.consumed);
  if (name.empty()) {
    AddReply(reply::FromError("ERR empty command"));
    return RequestStatus::kRejected;
  }
  RS_LOG_DEBUG("command name %.*s\n", static_cast<int>(name.size()),
               name.data());
  const auto* command = command::Find(name);
  if (command == nullptr) {
    RS_LOG_DEBUG("command not found\n");
    AddReply(reply::UnknownCommand(name));
    return RequestStatus::kRejected;
  }
  if (!command->arity.Accepts(args_.size())) {
    AddReply(reply::kWrongNumberOfArguments);
    return RequestStatus::kRejected;
  }
  command_ = command;
  return RequestStatus::kReady;
}

ClientStatus Client::ProcessCommand() {
  if (command_ == nullptr) {
    return ClientStatus::kError;
  }
  RS_LOG_DEBUG("process command: %.*s\n",
               static_cast<int>(command_->name.size()), command_->name.data());
  modified_ = false;
  {
    const CommandTimeScope command_time(db_);
    command_->callback(this);
  }
  if (modified_ && aof_ != nullptr &&
      !aof_->Append(command_->name, args_, db_)) {
    RS_LOG_DEBUG("failed to append command to AOF\n");
    close_after_reply_ = true;
    return ClientStatus::kError;
  }
  return ClientStatus::kOk;
}

bool Client::ExecuteForReplay(const command::Command* const command,
                              command::CommandArgs* const args) {
  if (command == nullptr || args == nullptr) {
    return false;
  }
  command_ = command;
  args_.swap(*args);
  modified_ = false;
  reply_error_ = false;
  command_->callback(this);
  args_.clear();
  args_.swap(*args);
  return !reply_error_;
}
}  // namespace redis_simple
