#include "server/client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "server/aof/aof.h"
#include "server/reply.h"
#include "server/request_parser.h"

namespace redis_simple {
namespace {
constexpr const size_t kReadBufferSize = size_t{16} * 1024;
// Upper bound for one read while receiving a large bulk argument.
constexpr const size_t kMaxBulkReadSize = size_t{1024} * 1024;
// Redis' default client-query-buffer-limit.
constexpr const size_t kMaxQueryBufferSize = size_t{1024} * 1024 * 1024;
constexpr std::string_view kQueued = "+QUEUED\r\n";
constexpr std::string_view kExecAbort =
    "-EXECABORT Transaction discarded because of previous errors.\r\n";

// Pins the database clock for one command.
class CommandTimeScope {
 public:
  explicit CommandTimeScope(db::RedisDb* const db) : db_(db) {
    db_->PinCommandTime();
  }
  CommandTimeScope(const CommandTimeScope&) = delete;
  CommandTimeScope& operator=(const CommandTimeScope&) = delete;
  CommandTimeScope(CommandTimeScope&&) = delete;
  CommandTimeScope& operator=(CommandTimeScope&&) = delete;
  ~CommandTimeScope() { db_->UnpinCommandTime(); }

 private:
  db::RedisDb* const db_;
};
}  // namespace

Client::Client(std::unique_ptr<connection::Connection> connection,
               const ServerContext* const server,
               OutputBufferLimits output_limits)
    : connection_(std::move(connection)),
      server_(server),
      output_limits_(output_limits),
      authenticated_(server->required_password.empty()) {}

Client::Client(const ServerContext* const server)
    : server_(server), discard_replies_(true) {}

Client::~Client() { Db()->UnwatchKeys(&watcher_); }

uint64_t Client::NextId() {
  // Clients are created on the main thread and on I/O threads.
  static std::atomic<uint64_t> next_id{1};
  return next_id.fetch_add(1, std::memory_order_relaxed);
}

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
  parse_state_.ReleaseUnescaped();
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
  if (framed == RequestStatus::kRejected) {
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
  // Redis bounds request sizes until the client authenticates.
  parse_state_.authenticated = authenticated_;
  const request_parser::ParseResult parsed =
      request_parser::Parse(query_buffer_.View(), &parse_state_, name, &args_);
  if (parsed.status == request_parser::ParseStatus::kIncomplete) {
    return RequestStatus::kIncomplete;
  }
  if (parsed.status == request_parser::ParseStatus::kInvalid) {
    // The message is built only when the error is replied to.
    protocol_failure_ = parsed;
    return RequestStatus::kProtocolError;
  }
  query_buffer_.Consume(parsed.consumed);
  // Empty requests are skipped without a reply, as in Redis.
  return parsed.empty_request ? RequestStatus::kRejected
                              : RequestStatus::kReady;
}

void Client::FailProtocol() {
  AddReply(reply::FromError(
      "ERR " + request_parser::ProtocolErrorMessage(protocol_failure_)));
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
    if (framed == RequestStatus::kRejected) {
      continue;
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
  parse_state_.ReleaseUnescaped();
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
  const commands::Command* command = commands::Find(name);
  if (command == nullptr) {
    RS_LOG_DEBUG("command not found\n");
    AddReply(reply::UnknownCommand(name, args_));
    multi_.MarkRejected();
    return RequestStatus::kRejected;
  }
  if (!command->arity.Accepts(args_.size())) {
    AddReply(reply::WrongNumberOfArguments(command->name));
    multi_.MarkRejected();
    return RequestStatus::kRejected;
  }
  if (!authenticated_ && !command->allowed_before_auth) {
    AddReply(reply::kNoAuthError);
    multi_.MarkRejected();
    return RequestStatus::kRejected;
  }
  if (command->access == commands::CommandAccess::kWrite &&
      !AofAcceptsWrites()) {
    AddReply(AofWriteError());
    multi_.MarkRejected();
    return RequestStatus::kRejected;
  }
  command_ = command;
  return RequestStatus::kReady;
}

bool Client::AofAcceptsWrites() const {
  return server_->aof == nullptr || server_->aof->AcceptingWrites();
}

std::string Client::AofWriteError() const {
  // Redis' reply while it cannot write the append-only file.
  return reply::FromError("MISCONF Errors writing to the AOF file: " +
                          server_->aof->LastErrorText());
}

bool Client::PropagateMutation(const commands::Command& command) {
  if (!modified_) {
    return true;
  }
  modified_ = false;
  // Key-level database operations touch WATCHed keys themselves; this covers
  // single-key commands that edit a value in place.
  const commands::KeySpec& keys = command.keys;
  if (keys.HasKeys() && keys.first == keys.last && keys.first < args_.size() &&
      Db()->HasWatchedKeys()) {
    Db()->TouchWatchedKey(args_[keys.first]);
  }
  if (Aof() != nullptr && !Aof()->Append(command.name, args_, Db())) {
    RS_LOG_DEBUG("failed to append command to AOF\n");
    close_after_reply_ = true;
    return false;
  }
  return true;
}

ClientStatus Client::ProcessCommand() {
  if (command_ == nullptr) {
    return ClientStatus::kError;
  }
  if (multi_.Active() && command_->queued_in_multi) {
    QueueCommand();
    return ClientStatus::kOk;
  }
  modified_ = false;
  {
    const CommandTimeScope command_time(Db());
    command_->callback(this);
  }
  if (!PropagateMutation(*command_) || propagation_failed_) {
    propagation_failed_ = false;
    return ClientStatus::kError;
  }
  return ClientStatus::kOk;
}

void Client::QueueCommand() {
  // After a rejected command EXEC aborts anyway, so stop copying arguments.
  if (!multi_.Failed() && !multi_.Queue(command_, args_)) {
    AddReply(reply::FromError("ERR MULTI queue limit exceeded"));
    return;
  }
  AddReply(kQueued);
}

void Client::DiscardTransaction() {
  multi_.Reset();
  UnwatchKeys();
}

void Client::ExecTransaction() {
  if (multi_.Failed()) {
    AddReply(kExecAbort);
    DiscardTransaction();
    return;
  }
  // The AOF may have failed after the writes were queued.
  if (!AofAcceptsWrites() && multi_.HasWriteCommand()) {
    AddReply(AofWriteError());
    DiscardTransaction();
    return;
  }
  // Looking the keys up resolves lazy expiration, so a WATCHed key that
  // expired since WATCH counts as modified.
  for (const std::string& key : watcher_.Keys()) {
    Db()->LookupKey(key);
  }
  if (watcher_.Dirty()) {
    AddReply(reply::NullArrayView(protocol_));
    DiscardTransaction();
    return;
  }
  UnwatchKeys();

  AddReply(reply::FromArrayHeader(multi_.Size()));
  const commands::Command* const exec_command = command_;
  if (Aof() != nullptr) {
    Aof()->BeginTransaction();
  }
  // Queued commands share the EXEC command's pinned clock, so the whole
  // transaction observes one time.
  multi_.ForEach(&args_, [this](const commands::Command* const queued) {
    command_ = queued;
    modified_ = false;
    command_->callback(this);
    if (!PropagateMutation(*command_)) {
      propagation_failed_ = true;
      return false;
    }
    return true;
  });
  if (Aof() != nullptr) {
    Aof()->EndTransaction();
  }
  command_ = exec_command;
  args_.clear();
  modified_ = false;
  multi_.Reset();
}

bool Client::ExecuteForReplay(const commands::Command* const command,
                              commands::CommandArgs* const args) {
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
