#ifndef REDIS_SIMPLE_SERVER_CLIENT_H_
#define REDIS_SIMPLE_SERVER_CLIENT_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "memory/dynamic_buffer.h"
#include "memory/reply_buffer.h"
#include "server/commands/command.h"
#include "server/db/db.h"
#include "server/db/key_watcher.h"
#include "server/reply.h"
#include "server/request_parser.h"

namespace redis_simple::aof {
class Aof;
}

namespace redis_simple {
class IoThread;

enum class ClientStatus {
  kOk = 0,
  kError = -1,
};

struct OutputBufferLimits {
  size_t resume_bytes{size_t{4} * 1024 * 1024};
  size_t soft_bytes{size_t{8} * 1024 * 1024};
  size_t hard_bytes{size_t{64} * 1024 * 1024};
};

class Client {
 public:
  // `db` must outlive the client; `aof` is null when persistence is off.
  static std::unique_ptr<Client> Create(
      std::unique_ptr<connection::Connection> connection, db::RedisDb* db,
      aof::Aof* aof, OutputBufferLimits output_limits = {}) {
    return std::unique_ptr<Client>(
        new Client(std::move(connection), db, aof, output_limits));
  }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client();
  connection::Connection* Connection() { return connection_.get(); }
  const connection::Connection* Connection() const { return connection_.get(); }
  // Never null: every client is bound to a database.
  db::RedisDb* Db() { return db_; }
  aof::Aof* Aof() { return aof_; }
  ssize_t ReadQuery();
  ssize_t SendReply();
  size_t AddReply(std::string_view reply);
  size_t AddReply(std::string&& reply);
  size_t AddReply(std::string&& header, std::string&& body);
  // Encodes `value` as a bulk string straight into the output buffer.
  size_t AddBulkReply(std::string_view value);
  bool HasPendingReplies() const { return !reply_buffer_.Empty(); }
  size_t PendingReplyBytes() const { return reply_buffer_.PendingBytes(); }
  bool ShouldPauseReads() const;
  bool ShouldResumeReads() const;
  bool ReadsPaused() const { return reads_paused_; }
  void SetReadsPaused(bool paused) { reads_paused_ = paused; }
  bool ShouldCloseAfterReply() const { return close_after_reply_; }
  void CloseAfterReply() { close_after_reply_ = true; }
  reply::ProtocolVersion Protocol() const { return protocol_; }
  void SetProtocol(reply::ProtocolVersion protocol) { protocol_ = protocol; }
  // Parses and runs every complete request in the query buffer.
  ClientStatus ProcessInputBuffer();
  // The same work split for I/O threads. ParseQueries, on the client's I/O
  // thread, frames the complete requests without running them and returns
  // whether there is anything for the main thread; RunParsedQueries, on the
  // main thread, validates and runs them. The parsed arguments view the query
  // buffer, which neither thread touches in between.
  bool ParseQueries();
  ClientStatus RunParsedQueries();
  void Free() {
    connection_->Close();
    freed_ = true;
  }
  // Whether the server has removed this client; it is destroyed later.
  bool Freed() const { return freed_; }
  // The server's I/O threads, for INFO; null without a server.
  const std::vector<std::unique_ptr<IoThread>>* IoThreads() const {
    return io_threads_;
  }
  void SetIoThreads(const std::vector<std::unique_ptr<IoThread>>* threads) {
    io_threads_ = threads;
  }
  const command::CommandArgs& Args() const { return args_; }
  // Requires AUTH before most commands. `password` must outlive the client.
  void RequirePassword(std::string_view password) {
    required_password_ = password;
    authenticated_ = password.empty();
  }
  std::string_view RequiredPassword() const { return required_password_; }
  bool Authenticated() const { return authenticated_; }
  void SetAuthenticated(bool authenticated) { authenticated_ = authenticated; }
  // CLIENT ID: unique for the life of the process, as in Redis.
  uint64_t Id() const { return id_; }
  // CLIENT SETNAME and HELLO SETNAME; empty means unnamed.
  std::string_view Name() const { return name_; }
  void SetName(std::string_view name) { name_.assign(name); }
  void MarkModified() { modified_ = true; }
  // MULTI/EXEC. Commands read while a transaction is open are copied into a
  // queue and run back to back by EXEC.
  bool InTransaction() const { return in_multi_; }
  void BeginTransaction() { in_multi_ = true; }
  // Drops the queued commands and every WATCHed key.
  void DiscardTransaction();
  // Runs the queued commands, or aborts when a command was rejected while
  // queueing or a WATCHed key changed; either way the transaction ends.
  void ExecTransaction();
  void WatchKey(std::string_view key) { db_->WatchKey(key, &watcher_); }
  void UnwatchKeys() { db_->UnwatchKeys(&watcher_); }

 private:
  friend class aof::Aof;
  enum class RequestStatus {
    kReady,
    kIncomplete,
    kRejected,
    kProtocolError,
  };

  Client(std::unique_ptr<connection::Connection> connection, db::RedisDb* db,
         aof::Aof* aof, OutputBufferLimits output_limits);
  explicit Client(db::RedisDb* db);
  bool ExecuteForReplay(const command::Command* command,
                        command::CommandArgs* args);
  // A command queued by MULTI. Its arguments are the `arg_count` entries of
  // `multi_arg_ends_` before `arg_end`, which index into `multi_args_`.
  struct QueuedCommand {
    const command::Command* command;
    size_t arg_end;
    size_t arg_count;
  };

  RequestStatus ParseRequest();
  // RESP framing only: fills `args_` and consumes the request, with no
  // reply or other side effect.
  RequestStatus FrameRequest(std::string_view* name);
  // Command lookup, arity, AUTH, and MISCONF checks; sets `command_`.
  RequestStatus ValidateRequest(std::string_view name);
  void FailProtocol();
  // False while the AOF cannot store writes; write commands are then
  // rejected with AofWriteError(), as Redis replies with MISCONF.
  bool AofAcceptsWrites() const;
  std::string AofWriteError() const;
  ClientStatus ProcessCommand();
  void QueueCommand();
  void ResetTransaction();
  // Propagates a successful mutation: touches WATCHed keys and appends the
  // command to the AOF. Returns false if the AOF append failed.
  bool PropagateMutation(const command::Command& command);
  bool CanQueueReply(size_t size);
  ssize_t SendBufferReply();
  ssize_t SendListReply();
  std::unique_ptr<connection::Connection> connection_;
  db::RedisDb* db_{nullptr};
  aof::Aof* aof_{nullptr};
  const command::Command* command_{nullptr};
  command::CommandArgs args_;
  // Requests framed by ParseQueries: each is `arg_count` arguments after its
  // name at `first` in `parsed_args_`.
  struct ParsedRequest {
    size_t first;
    size_t arg_count;
  };
  std::vector<ParsedRequest> parsed_requests_;
  std::vector<std::string_view> parsed_args_;
  bool protocol_error_{};
  request_parser::ParseResult protocol_failure_;
  request_parser::ParseState parse_state_;
  in_memory::DynamicBuffer query_buffer_;
  in_memory::ReplyBuffer reply_buffer_;
  std::vector<iovec> reply_blocks_;
  OutputBufferLimits output_limits_;
  reply::ProtocolVersion protocol_{reply::ProtocolVersion::kResp2};
  bool reads_paused_{};
  bool freed_{};
  const std::vector<std::unique_ptr<IoThread>>* io_threads_{nullptr};
  bool close_after_reply_{};
  bool modified_{};
  bool propagation_failed_{};
  bool in_multi_{};
  bool multi_error_{};
  std::vector<QueuedCommand> multi_commands_;
  std::vector<size_t> multi_arg_ends_;
  std::string multi_args_;
  db::KeyWatcher watcher_;
  bool discard_replies_{};
  bool reply_error_{};
  std::string_view required_password_;
  bool authenticated_{true};
  uint64_t id_{NextId()};
  std::string name_;

  static uint64_t NextId();
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_CLIENT_H_
