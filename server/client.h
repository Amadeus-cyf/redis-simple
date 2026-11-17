#ifndef REDIS_SIMPLE_SERVER_CLIENT_H_
#define REDIS_SIMPLE_SERVER_CLIENT_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "containers/dynamic_buffer.h"
#include "containers/reply_buffer.h"
#include "server/commands/command.h"
#include "server/db/db.h"
#include "server/db/key_watcher.h"
#include "server/multi_state.h"
#include "server/reply.h"
#include "server/request_parser.h"
#include "server/server_context.h"

namespace redis_simple::aof {
class FileReplayer;
}

namespace redis_simple {
enum class ClientStatus {
  kOk = 0,
  kError = -1,
};

// Bounds on a client's unsent reply bytes. The server stops reading from a
// client with `soft_bytes` pending (0 turns that off) and reads again once
// at most `resume_bytes` are left. A reply that would take the pending bytes
// past `hard_bytes` is dropped, and the client is closed.
struct OutputBufferLimits {
  size_t resume_bytes{size_t{4} * 1024 * 1024};
  size_t soft_bytes{size_t{8} * 1024 * 1024};
  size_t hard_bytes{size_t{64} * 1024 * 1024};
};

// One connection's state: its query and reply buffers, the request being
// parsed and run, its RESP version, and its MULTI and WATCH state. A client
// belongs to one thread at a time (see HandedToMain).
class Client {
 public:
  // `server` must outlive the client. The client starts authenticated
  // unless the server requires a password.
  static std::unique_ptr<Client> Create(
      std::unique_ptr<connection::Connection> connection,
      const ServerContext* server, OutputBufferLimits output_limits = {}) {
    return std::unique_ptr<Client>(
        new Client(std::move(connection), server, output_limits));
  }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client();
  connection::Connection* Connection() { return connection_.get(); }
  const connection::Connection* Connection() const { return connection_.get(); }
  const ServerContext& Server() const { return *server_; }
  // Never null: every client is bound to a database.
  db::RedisDb* Db() { return server_->db; }
  // Null when persistence is off.
  aof::Aof* Aof() { return server_->aof; }
  // Reads once from the socket into the query buffer. Returns the bytes
  // read, 0 when the peer closed or the read would block, or -1 when the
  // connection failed or the query buffer passed its size limit.
  ssize_t ReadQuery();
  // Writes pending reply bytes to the socket once. Returns the bytes written,
  // or -1 when the write failed or the socket would block; only a failure
  // changes the connection's State().
  ssize_t SendReply();
  // The reply functions return the bytes queued, or 0 when the reply would
  // pass the output buffer's hard limit; the client then closes after the
  // replies already queued are sent.
  size_t AddReply(std::string_view reply);
  // Encode the value on the stack (0 and 1 are constant replies) and copy
  // it straight into the output buffer. The float is a bulk string in RESP2 and
  // a double in RESP3.
  size_t AddIntegerReply(int64_t value);
  size_t AddFloatReply(double value);
  size_t AddReply(std::string&& reply);
  size_t AddReply(std::string&& header, std::string&& body);
  // Encodes `value` as a bulk string straight into the output buffer.
  size_t AddBulkReply(std::string_view value);
  // The same reply for a value held behind a shared pointer, which must not
  // be null. A large value is queued by reference and sent without a copy;
  // the reply keeps it alive, and unchanged, until it is flushed.
  size_t AddBulkReply(std::shared_ptr<const std::string> value);
  bool HasPendingReplies() const { return !reply_buffer_.Empty(); }
  // Frees the buffers of a client that has read nothing since the previous
  // call: the query buffer and argument lists if it holds no unparsed input,
  // and the reply buffer and block list if it holds no unsent reply.
  // Otherwise one large request or reply would keep its memory for as long
  // as the connection lives; Redis' clientsCron shrinks idle clients'
  // buffers for the same reason. Only the side that owns the client may
  // call this.
  void ReleaseIdleBuffers();
  // Bytes allocated for the query buffer and the reply buffer's main block.
  size_t BufferCapacity() const {
    return query_buffer_.Capacity() + reply_buffer_.BufferCapacity();
  }
  // The client's place in its IdleClientSweeper, which alone sets it.
  static constexpr size_t kNotSwept = std::numeric_limits<size_t>::max();
  size_t SweepIndex() const { return sweep_index_; }
  void SetSweepIndex(size_t index) { sweep_index_ = index; }
  size_t PendingReplyBytes() const { return reply_buffer_.PendingBytes(); }
  bool ShouldPauseReads() const;
  bool ShouldResumeReads() const;
  bool ReadsPaused() const { return reads_paused_; }
  void SetReadsPaused(bool paused) { reads_paused_ = paused; }
  // Whether the client's I/O thread has handed it to the main thread to run
  // its commands. Only that I/O thread reads or writes this.
  bool HandedToMain() const { return handed_to_main_; }
  void SetHandedToMain(bool handed) { handed_to_main_ = handed; }
  // Whether the server holds this client's replies until it has flushed the
  // AOF for the commands of this event-loop iteration. Main thread only.
  bool AwaitsFlush() const { return awaits_flush_; }
  void SetAwaitsFlush(bool awaits) { awaits_flush_ = awaits; }
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
  // Closes the connection and marks the client for the server to destroy.
  void Free() {
    connection_->Close();
    freed_ = true;
  }
  // Whether the server has removed this client; it is destroyed later.
  bool Freed() const { return freed_; }
  const commands::CommandArgs& Args() const { return args_; }
  bool Authenticated() const { return authenticated_; }
  void SetAuthenticated(bool authenticated) { authenticated_ = authenticated; }
  // CLIENT ID: unique for the life of the process, as in Redis.
  uint64_t Id() const { return id_; }
  // CLIENT SETNAME and HELLO SETNAME; empty means unnamed.
  std::string_view Name() const { return name_; }
  void SetName(std::string_view name) { name_.assign(name); }
  // Records that the command changed the database, so it reaches WATCHers
  // and the AOF. `changes` counts toward the changes since the last RDB save
  // as Redis counts them in server.dirty: one per key, field, member, or
  // element added, removed, or set.
  void MarkModified(int64_t changes = 1) {
    modified_ = true;
    *change_counter_ += changes;
  }
  // MULTI/EXEC. Commands read while a transaction is open are copied into a
  // queue and run back to back by EXEC.
  bool InTransaction() const { return multi_.Active(); }
  void BeginTransaction() { multi_.Begin(); }
  // Drops the queued commands and every WATCHed key.
  void DiscardTransaction();
  // Runs the queued commands, or aborts when a command was rejected while
  // queueing or a WATCHed key changed; either way the transaction ends.
  void ExecTransaction();
  void WatchKey(std::string_view key) { Db()->WatchKey(key, &watcher_); }
  void UnwatchKeys() { Db()->UnwatchKeys(&watcher_); }

 private:
  // Replays AOF commands through a client without a connection.
  friend class aof::FileReplayer;
  enum class RequestStatus {
    kReady,
    kIncomplete,
    kRejected,
    kProtocolError,
  };

  Client(std::unique_ptr<connection::Connection> connection,
         const ServerContext* server, OutputBufferLimits output_limits);
  // A client for AOF replay, without a connection; it discards replies.
  explicit Client(const ServerContext* server);
  bool ExecuteForReplay(const commands::Command* command,
                        commands::CommandArgs* args);
  RequestStatus ParseRequest();
  // RESP framing only: fills `args_` and consumes the request, with no
  // reply or other side effect.
  RequestStatus FrameRequest(std::string_view* name);
  // Command lookup, arity, AUTH, and MISCONF checks; sets `command_`.
  RequestStatus ValidateRequest(std::string_view name);
  void FailProtocol();
  // Redis' writeCommandsDeniedByDiskError: true after a failed background
  // save (see rdb::Rdb::DeniesWrites) or while the AOF cannot store writes.
  // Write commands and PING are then refused with DiskErrorMessage().
  bool WritesRefused() const;
  std::string DiskErrorMessage() const;
  ClientStatus ProcessCommand();
  void QueueCommand();
  // Propagates a successful mutation: touches WATCHed keys and appends the
  // command to the AOF. Returns false if the AOF append failed. Inlined into
  // the per-command path.
  [[gnu::always_inline]] inline bool PropagateMutation(
      const commands::Command& command);
  bool CanQueueReply(size_t size);
  ssize_t SendBufferReply();
  ssize_t SendListReply();
  std::unique_ptr<connection::Connection> connection_;
  const ServerContext* server_;
  const commands::Command* command_{nullptr};
  commands::CommandArgs args_;
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
  containers::DynamicBuffer query_buffer_;
  containers::ReplyBuffer reply_buffer_;
  std::vector<iovec> reply_blocks_;
  OutputBufferLimits output_limits_;
  reply::ProtocolVersion protocol_{reply::ProtocolVersion::kResp2};
  bool read_since_sweep_{};
  size_t sweep_index_{kNotSwept};
  bool reads_paused_{};
  bool handed_to_main_{};
  bool freed_{};
  bool awaits_flush_{};
  bool close_after_reply_{};
  bool modified_{};
  // MarkModified's changes when the server has no RDB counter, as in AOF
  // replay, which reads them from here.
  int64_t changes_{};
  // The server's change counter (ServerContext::changes), or `changes_`.
  int64_t* change_counter_;
  bool propagation_failed_{};
  MultiState multi_;
  db::KeyWatcher watcher_;
  bool discard_replies_{};
  bool reply_error_{};
  bool authenticated_{true};
  uint64_t id_{NextId()};
  std::string name_;

  static uint64_t NextId();
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_CLIENT_H_
