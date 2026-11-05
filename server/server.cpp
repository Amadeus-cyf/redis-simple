#include "server/server.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "event_loop/file_event.h"
#include "event_loop/time_event.h"
#include "logging/logger.h"
#include "server/client_connection/client_connection.h"
#include "server/client_connection/read_query.h"
#include "server/client_connection/write_reply.h"
#include "server/db/db.h"
#include "server/expire.h"
#include "server/shutdown.h"

namespace redis_simple {
Server::Server()
    : loop_(event_loop::Loop::Create()), db_(db::RedisDb::Create()) {}

std::unique_ptr<Server> Server::Create() {
  return std::unique_ptr<Server>(new Server());
}

bool Server::Run(const ServerOptions& options) {
  if (loop_ == nullptr || options.bind_address.empty() || options.port <= 0 ||
      options.port > 65535 || !shutdown::InstallSignalHandlers()) {
    return false;
  }
  aof_.reset();
  io_stats_ = IoStats{};
  require_pass_ = options.require_pass;
  protected_mode_ = options.protected_mode;
  db_ = db::RedisDb::Create();
  if (db_ == nullptr) {
    return false;
  }
  if (options.append_only) {
    aof_ = aof::Aof::Open(options.aof_options, db_.get());
    if (aof_ == nullptr) {
      return false;
    }
  }
  connection::Connection connection(
      connection::Context{.loop = loop_.get(), .fd = -1});
  const connection::AddressInfo address(options.bind_address, options.port);
  if (connection.BindAndListen(address) ==
      connection::ConnectionStatus::kError) {
    aof_.reset();
    return false;
  }
  fd_ = connection.Descriptor();
  if (options.io_threads > 1) {
    io_threads_ = IoThreads::Create(static_cast<size_t>(options.io_threads));
    if (io_threads_ == nullptr) {
      RS_LOG_WARN("failed to start %d I/O threads\n", options.io_threads);
      fd_ = -1;
      aof_.reset();
      return false;
    }
    loop_->SetBeforeSleep([this] { HandlePendingIo(); });
  }
  if (!InstallAcceptCallback()) {
    loop_->SetBeforeSleep(nullptr);
    io_threads_.reset();
    fd_ = -1;
    aof_.reset();
    return false;
  }
  loop_->CreateTimeEvent(event_loop::TimeEvent::Create(
      [this](int64_t /*id*/) { return ServerCron(); }, nullptr));
  loop_->Run();
  loop_->DeleteFileEvent(fd_, event_loop::EventFlag::kReadable);
  loop_->SetBeforeSleep(nullptr);
  fd_ = -1;
  pending_reads_.clear();
  pending_writes_.clear();
  io_threads_.reset();
  clients_.clear();
  aof_.reset();
  return true;
}

void Server::Stop() {
  if (loop_ != nullptr) {
    loop_->Stop();
  }
}

bool Server::RemoveClient(Client* c) {
  if (c == nullptr) {
    return false;
  }
  // The client is destroyed before the next batch, so it must leave the
  // queue. Clients in the batch being handled are skipped once freed.
  if (c->PendingRead()) {
    std::erase(pending_reads_, c);
    c->SetPendingRead(false);
  }
  c->Free();
  // Destroy the client after the current event, which may still use it.
  loop_->Defer([this, c] { clients_.erase(c); });
  return true;
}

void Server::QueueRead(Client* const client) {
  if (!client->PendingRead()) {
    client->SetPendingRead(true);
    pending_reads_.push_back(client);
  }
}

void Server::QueueWrite(Client* const client) {
  // A client with a write callback is waiting for its socket to drain and
  // writes from that event instead.
  if (client->HasPendingReplies() &&
      !client->Connection()->HasWriteCallback()) {
    pending_writes_.push_back(client);
  }
}

void Server::HandlePendingIo() {
  if (!pending_reads_.empty()) {
    ReadPendingClients();
  }
  if (!pending_writes_.empty()) {
    WritePendingClients();
  }
}

void Server::ReadPendingClients() {
  io_batch_.clear();
  io_batch_.swap(pending_reads_);
  for (Client* const client : io_batch_) {
    client->SetPendingRead(false);
  }
  io_results_.assign(io_batch_.size(), 0);
  auto read = [this](size_t index) {
    io_results_[index] = io_batch_[index]->ReadQuery();
  };
  if (io_threads_->Run(io_batch_.size(), read)) {
    io_stats_.threaded_reads += io_batch_.size();
  }
  io_stats_.active = io_threads_->Active();

  // Commands run here, one client after another in arrival order.
  size_t ran = 0;
  for (size_t index = 0; index < io_batch_.size(); ++index) {
    Client* const client = io_batch_[index];
    if (client_connection::RunQueries(this, client, io_results_[index])) {
      io_batch_[ran++] = client;
    }
  }
  io_batch_.resize(ran);
  // Persist the whole batch's mutations before any of its replies are sent.
  if (aof_ != nullptr && !io_batch_.empty() && !aof_->Flush()) {
    Stop();
    for (Client* const client : io_batch_) {
      if (!client->Freed()) {
        RemoveClient(client);
      }
    }
    io_batch_.clear();
    return;
  }
  for (Client* const client : io_batch_) {
    if (!client->Freed()) {
      client_connection::FinishQueries(this, client);
    }
  }
  io_batch_.clear();
}

void Server::WritePendingClients() {
  io_batch_.clear();
  io_batch_.swap(pending_writes_);
  io_results_.assign(io_batch_.size(), 0);
  auto write = [this](size_t index) {
    io_results_[index] =
        client_connection::WritePendingReplies(io_batch_[index]);
  };
  if (io_threads_->Run(io_batch_.size(), write)) {
    io_stats_.threaded_writes += io_batch_.size();
  }
  io_stats_.active = io_threads_->Active();
  for (size_t index = 0; index < io_batch_.size(); ++index) {
    Client* const client = io_batch_[index];
    if (!client->Freed()) {
      client_connection::FinishWrite(this, client, io_results_[index]);
    }
  }
  io_batch_.clear();
}

bool Server::InstallAcceptCallback() {
  auto file_event = event_loop::FileEvent::Create(
      client_connection::AcceptConnectionCallback, nullptr, this,
      event_loop::ToInt(event_loop::EventFlag::kReadable));
  if (loop_->CreateFileEvent(fd_, std::move(file_event)) ==
      event_loop::Status::kError) {
    RS_LOG_DEBUG("error in adding client creation file event\n");
    return false;
  }
  return true;
}

int64_t Server::ServerCron() {
  constexpr int64_t kCronIntervalMs = 100;
  if (shutdown::StopRequested() || (aof_ != nullptr && !aof_->Healthy())) {
    Stop();
    return event_loop::TimeEvent::kNoMore;
  }
  if (aof_ != nullptr && aof_->ShouldAutoRewrite() &&
      aof_->StartRewrite(db_.get()) == aof::RewriteResult::kError) {
    RS_LOG_WARN("automatic AOF rewrite failed to start\n");
  }
  ActiveExpireCycle(db_.get());
  return kCronIntervalMs;
}
}  // namespace redis_simple
