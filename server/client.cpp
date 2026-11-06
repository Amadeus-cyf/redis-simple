#include "server/client.h"

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
constexpr const size_t kReadBufferSize = size_t{16} * 1024;
// Upper bound for one read while receiving a large bulk argument.
constexpr const size_t kMaxBulkReadSize = size_t{1024} * 1024;
constexpr const size_t kMaxQueryBufferSize = size_t{64} * 1024 * 1024;
// Upper bound for the argument bytes one MULTI may queue.
constexpr const size_t kMaxTransactionBytes = kMaxQueryBufferSize;
// Queue buffers larger than this are released after a transaction ends.
constexpr const size_t kRetainedTransactionBytes = size_t{64} * 1024;
constexpr std::string_view kQueued = "+QUEUED\r\n";
constexpr std::string_view kExecAbort =
    "-EXECABORT Transaction discarded because of previous errors.\r\n";

// Pins the database clock for one command.
class CommandTimeScope {
 public:
  explicit CommandTimeScope(db::RedisDb* const db) : db_(db) {
    db_->SetCommandTime(utils::NowInMilliseconds());
  }
  CommandTimeScope(const CommandTimeScope&) = delete;
  CommandTimeScope& operator=(const CommandTimeScope&) = delete;
  CommandTimeScope(CommandTimeScope&&) = delete;
  CommandTimeScope& operator=(CommandTimeScope&&) = delete;
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

Client::~Client() { db_->UnwatchKeys(&watcher_); }

size_t Client::AddReply(std::string_view reply_text) {
  if (discard_replies_) {
    reply_error_ =
        reply_error_ || (!reply_text.empty() && reply_text.front() == '-');
    return reply_text.size();
  }
  if (!CanQueueReply(reply_text.size())) {
    return 0;
  }
  return reply_buffer_.Append(reply_text.data(), reply_text.size());
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
  return reply_buffer_.Append(std::move(reply_text));
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
  const size_t header_size = reply_buffer_.Append(std::move(header));
  const size_t body_size = reply_buffer_.Append(std::move(body));
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
  reply_buffer_.Append(header.data(), header_size);
  reply_buffer_.Append(value.data(), value.size());
  reply_buffer_.Append(kCrlf.data(), kCrlf.size());
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
      parse_state_.PendingBulkBytes(query_buffer_.View().size());
  const size_t read_size =
      std::clamp(pending_bulk, kReadBufferSize, kMaxBulkReadSize);
  char* const tail = query_buffer_.WritableTail(read_size);
  const ssize_t nread = connection_->Read({tail, read_size});
  if (nread == 0) {
    return 0;
  }
  if (nread < 0) {
    // Would-block leaves the connection usable; other errors mark it failed.
    return connection_->State() == connection::ConnectionState::kConnected ? 0
                                                                           : -1;
  }
  query_buffer_.Commit(static_cast<size_t>(nread));
  if (query_buffer_.View().size() > kMaxQueryBufferSize) {
    RS_LOG_DEBUG("query buffer limit exceeded\n");
    connection_->SetState(connection::ConnectionState::kError);
    return -1;
  }
  return nread;
}

ssize_t Client::SendReply() {
  return reply_buffer_.ReplyCount() > 0 ? SendListReply() : SendBufferReply();
}

ssize_t Client::SendBufferReply() {
  const size_t reply_length = reply_buffer_.UnsentLength();
  const int logged_length = static_cast<int>(std::min(
      reply_length, static_cast<size_t>(std::numeric_limits<int>::max())));
  RS_LOG_DEBUG("SendBufferReply %.*s %zu\n", logged_length,
               reply_buffer_.UnsentBuffer(), reply_length);
  ssize_t nwritten = connection_->Write(
      {reply_buffer_.UnsentBuffer(), reply_buffer_.UnsentLength()});
  if (nwritten < 0) {
    return -1;
  }
  reply_buffer_.Consume(nwritten);
  return nwritten;
}

ssize_t Client::SendListReply() {
  RS_LOG_DEBUG("SendListReply\n");
  reply_buffer_.FillBlocks(&reply_blocks_);
  ssize_t nwritten = connection_->WriteVector(reply_blocks_);
  if (nwritten < 0) {
    return -1;
  }
  reply_buffer_.Consume(nwritten);
  return nwritten;
}

ClientStatus Client::ProcessInputBuffer() {
  while (!close_after_reply_ &&
         query_buffer_.Consumed() < query_buffer_.Size()) {
    RS_LOG_DEBUG("process loop %zu %zu\n", query_buffer_.Consumed(),
                 query_buffer_.Size());
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
  query_buffer_.Compact();
  return ClientStatus::kOk;
}

Client::RequestStatus Client::ParseRequest() {
  std::string_view name;
  const RequestStatus framed = FrameRequest(&name);
  if (framed == RequestStatus::kProtocolError) {
    FailProtocol();
    return framed;
  }
  if (framed != RequestStatus::kReady) {
    return framed;
  }
  return ValidateRequest(name);
}

Client::RequestStatus Client::FrameRequest(std::string_view* const name) {
  command_ = nullptr;
  args_.clear();
  const request_parser::ParseResult parsed =
      request_parser::Parse(query_buffer_.View(), &parse_state_, name, &args_);
  if (parsed.status == request_parser::ParseStatus::kIncomplete) {
    return RequestStatus::kIncomplete;
  }
  if (parsed.status == request_parser::ParseStatus::kInvalid) {
    return RequestStatus::kProtocolError;
  }
  query_buffer_.Consume(parsed.consumed);
  return RequestStatus::kReady;
}

void Client::FailProtocol() {
  AddReply(reply::FromError("ERR Protocol error: invalid request"));
  close_after_reply_ = true;
  query_buffer_.Clear();
}

bool Client::ParseQueries() {
  parsed_args_.clear();
  parsed_requests_.clear();
  while (!protocol_error_ && query_buffer_.Consumed() < query_buffer_.Size()) {
    std::string_view name;
    const RequestStatus framed = FrameRequest(&name);
    if (framed == RequestStatus::kIncomplete) {
      break;
    }
    if (framed == RequestStatus::kProtocolError) {
      // Replied to on the main thread, after the requests before it.
      protocol_error_ = true;
      break;
    }
    parsed_requests_.push_back({parsed_args_.size(), args_.size()});
    parsed_args_.push_back(name);
    parsed_args_.insert(parsed_args_.end(), args_.begin(), args_.end());
  }
  return !parsed_requests_.empty() || protocol_error_;
}

ClientStatus Client::RunParsedQueries() {
  ClientStatus status = ClientStatus::kOk;
  for (const ParsedRequest& request : parsed_requests_) {
    if (close_after_reply_) {
      break;
    }
    const auto first =
        parsed_args_.begin() + static_cast<std::ptrdiff_t>(request.first);
    args_.assign(first + 1,
                 first + 1 + static_cast<std::ptrdiff_t>(request.arg_count));
    if (ValidateRequest(*first) != RequestStatus::kReady) {
      continue;
    }
    if (ProcessCommand() == ClientStatus::kError) {
      status = ClientStatus::kError;
      break;
    }
  }
  parsed_requests_.clear();
  parsed_args_.clear();
  if (protocol_error_) {
    protocol_error_ = false;
    if (!close_after_reply_) {
      FailProtocol();
    }
  }
  query_buffer_.Compact();
  return status;
}

Client::RequestStatus Client::ValidateRequest(std::string_view name) {
  if (name.empty()) {
    AddReply(reply::FromError("ERR empty command"));
    return RequestStatus::kRejected;
  }
  RS_LOG_DEBUG("command name %.*s\n", static_cast<int>(name.size()),
               name.data());
  const command::Command* command = command::Find(name);
  if (command == nullptr) {
    RS_LOG_DEBUG("command not found\n");
    AddReply(reply::UnknownCommand(name, args_));
    multi_error_ = in_multi_;
    return RequestStatus::kRejected;
  }
  if (!command->arity.Accepts(args_.size())) {
    AddReply(reply::WrongNumberOfArguments(command->name));
    multi_error_ = in_multi_;
    return RequestStatus::kRejected;
  }
  if (!authenticated_ && !command->allowed_before_auth) {
    AddReply(reply::kNoAuthError);
    multi_error_ = in_multi_;
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
  if (in_multi_ && command_->queued_in_multi) {
    QueueCommand();
    return ClientStatus::kOk;
  }
  modified_ = false;
  {
    const CommandTimeScope command_time(db_);
    command_->callback(this);
  }
  if (!PropagateMutation(*command_) || propagation_failed_) {
    propagation_failed_ = false;
    return ClientStatus::kError;
  }
  return ClientStatus::kOk;
}

bool Client::PropagateMutation(const command::Command& command) {
  if (!modified_) {
    return true;
  }
  modified_ = false;
  // Key-level database operations touch WATCHed keys themselves; this covers
  // single-key commands that edit a value in place.
  const command::KeySpec& keys = command.keys;
  if (keys.HasKeys() && keys.first == keys.last && keys.first < args_.size() &&
      db_->HasWatchedKeys()) {
    db_->TouchWatchedKey(args_[keys.first]);
  }
  if (aof_ != nullptr && !aof_->Append(command.name, args_, db_)) {
    RS_LOG_DEBUG("failed to append command to AOF\n");
    close_after_reply_ = true;
    return false;
  }
  return true;
}

void Client::QueueCommand() {
  if (multi_error_) {
    // EXEC will abort anyway; do not keep copying arguments.
    AddReply(kQueued);
    return;
  }
  size_t bytes = 0;
  for (const std::string_view arg : args_) {
    bytes += arg.size();
  }
  if (bytes > kMaxTransactionBytes - multi_args_.size()) {
    // Release the queue now; the flag makes EXEC abort the transaction.
    AddReply(reply::FromError("ERR MULTI queue limit exceeded"));
    ResetTransaction();
    in_multi_ = true;
    multi_error_ = true;
    return;
  }
  // The arguments view the query buffer, which is reused after this read, so
  // the queue owns a copy of them in one contiguous buffer.
  multi_args_.reserve(multi_args_.size() + bytes);
  for (const std::string_view arg : args_) {
    multi_args_.append(arg);
    multi_arg_ends_.push_back(multi_args_.size());
  }
  multi_commands_.push_back({command_, multi_arg_ends_.size(), args_.size()});
  AddReply(kQueued);
}

void Client::ResetTransaction() {
  in_multi_ = false;
  multi_error_ = false;
  multi_commands_.clear();
  multi_arg_ends_.clear();
  multi_args_.clear();
  if (multi_args_.capacity() > kRetainedTransactionBytes) {
    multi_args_.shrink_to_fit();
    multi_arg_ends_.shrink_to_fit();
    multi_commands_.shrink_to_fit();
  }
}

void Client::DiscardTransaction() {
  ResetTransaction();
  UnwatchKeys();
}

void Client::ExecTransaction() {
  if (multi_error_) {
    AddReply(kExecAbort);
    DiscardTransaction();
    return;
  }
  // Looking the keys up resolves lazy expiration, so a WATCHed key that
  // expired since WATCH counts as modified.
  for (const std::string& key : watcher_.Keys()) {
    db_->LookupKey(key);
  }
  if (watcher_.Dirty()) {
    AddReply(reply::NullArrayView(protocol_));
    DiscardTransaction();
    return;
  }
  UnwatchKeys();

  AddReply(reply::FromArrayHeader(multi_commands_.size()));
  const command::Command* const exec_command = command_;
  if (aof_ != nullptr) {
    aof_->BeginTransaction();
  }
  // Queued commands share the EXEC command's pinned clock, so the whole
  // transaction observes one time.
  for (const QueuedCommand& queued : multi_commands_) {
    command_ = queued.command;
    args_.clear();
    const size_t first_arg = queued.arg_end - queued.arg_count;
    size_t arg_start = first_arg == 0 ? 0 : multi_arg_ends_[first_arg - 1];
    for (size_t index = first_arg; index < queued.arg_end; ++index) {
      const size_t arg_end = multi_arg_ends_[index];
      args_.emplace_back(multi_args_.data() + arg_start, arg_end - arg_start);
      arg_start = arg_end;
    }
    modified_ = false;
    command_->callback(this);
    if (!PropagateMutation(*command_)) {
      propagation_failed_ = true;
      break;
    }
  }
  if (aof_ != nullptr) {
    aof_->EndTransaction();
  }
  command_ = exec_command;
  args_.clear();
  modified_ = false;
  ResetTransaction();
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
