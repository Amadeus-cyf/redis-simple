#include "server.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cstdint>
#include <utility>

#include "client_connection/client_connection.h"
#include "db/db.h"
#include "event_loop/file_event.h"
#include "event_loop/time_event.h"
#include "expire.h"
#include "logging/logger.h"
#include "server/shutdown.h"

namespace redis_simple {
Server::Server()
    : loop_(event_loop::Loop::Create()), db_(db::RedisDb::Create()) {}

Server* Server::Get() {
  static Server server;
  return &server;
}

bool Server::Run(const ServerOptions& options) {
  if (loop_ == nullptr || options.bind_address.empty() || options.port <= 0 ||
      options.port > 65535 || !shutdown::InstallSignalHandlers()) {
    return false;
  }
  aof_.reset();
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
  connection::Context ctx;
  ctx.loop = loop_.get();
  ctx.fd = -1;
  connection::Connection conn(ctx);
  const connection::AddressInfo addr_info(options.bind_address, options.port);
  if (conn.BindAndListen(addr_info) == connection::ConnectionStatus::kError) {
    aof_.reset();
    return false;
  }
  fd_ = conn.Descriptor();
  if (!InstallAcceptCallback()) {
    fd_ = -1;
    aof_.reset();
    return false;
  }
  loop_->CreateTimeEvent(event_loop::TimeEvent::Create(
      [this](int64_t /*id*/) { return ServerCron(); }, nullptr));
  loop_->Run();
  loop_->DeleteFileEvent(fd_, event_loop::EventFlag::kReadable);
  fd_ = -1;
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
  c->Free();
  // Destroy the client after the current event, which may still use it.
  loop_->Defer([this, c] { clients_.erase(c); });
  return true;
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
