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
    // Threads that allocate or free memory wait out the rewrite's fork: the
    // I/O threads, as they read and parse, and the database's thread that
    // frees UNLINKed values.
    aof_->SetForkHooks(
        [this] {
          PauseIoThreads();
          db_->PauseBackgroundFrees();
        },
        [this] {
          db_->ResumeBackgroundFrees();
          ResumeIoThreads();
        });
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
  // Like Redis, --io-threads counts the main thread, which runs commands
  // while the others own the client sockets.
  if (options.io_threads > 1 &&
      !StartIoThreads(static_cast<size_t>(options.io_threads) - 1)) {
    RS_LOG_WARN("failed to start %d I/O threads\n", options.io_threads - 1);
    StopIoThreads();
    fd_ = -1;
    aof_.reset();
    return false;
  }
  if (!InstallAcceptCallback()) {
    StopIoThreads();
    fd_ = -1;
    aof_.reset();
    return false;
  }
  loop_->CreateTimeEvent(event_loop::TimeEvent::Create(
      [this](int64_t /*id*/) { return ServerCron(); }, nullptr));
  loop_->Run();
  loop_->DeleteFileEvent(fd_, event_loop::EventFlag::kReadable);
  fd_ = -1;
  StopIoThreads();
  return true;
}

bool Server::StartIoThreads(size_t thread_count) {
  for (size_t index = 1; index <= thread_count; ++index) {
    std::unique_ptr<IoThread> thread = IoThread::Create(
        index, loop_.get(),
        [this](IoThread* const io_thread,
               std::vector<IoThread::Handoff>* const handoffs) {
          RunHandedOverClients(io_thread, handoffs);
        });
    if (thread == nullptr) {
      return false;
    }
    io_threads_.push_back(std::move(thread));
  }
  return true;
}

void Server::StopIoThreads() {
  // Stop the threads first, so freeing the clients below unregisters their
  // sockets from loops no thread runs anymore; the loops go last.
  for (const std::unique_ptr<IoThread>& thread : io_threads_) {
    thread->Stop();
  }
  clients_.clear();
  aof_.reset();
  io_threads_.clear();
}

IoThread* Server::NextIoThread() {
  IoThread* chosen = nullptr;
  for (const std::unique_ptr<IoThread>& thread : io_threads_) {
    if (chosen == nullptr || thread->ClientCount() < chosen->ClientCount()) {
      chosen = thread.get();
    }
  }
  return chosen;
}

void Server::PauseIoThreads() {
  for (const std::unique_ptr<IoThread>& thread : io_threads_) {
    thread->Pause();
  }
}

void Server::ResumeIoThreads() {
  for (const std::unique_ptr<IoThread>& thread : io_threads_) {
    thread->Resume();
  }
}

void Server::RunHandedOverClients(
    IoThread* const thread, std::vector<IoThread::Handoff>* const handoffs) {
  handed_over_.clear();
  for (const IoThread::Handoff& handoff : *handoffs) {
    if (handoff.closed) {
      thread->ClientFreed();
      RemoveClient(handoff.client);
      continue;
    }
    handoff.client->RunParsedQueries();
    handed_over_.push_back(handoff.client);
  }
  // Persist these clients' mutations before any of their replies are sent.
  if (aof_ != nullptr && !handed_over_.empty() && !aof_->Flush()) {
    Stop();
    return;
  }
  for (Client* const client : handed_over_) {
    thread->Return(client);
  }
  handed_over_.clear();
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
  if (shutdown::StopRequested()) {
    Stop();
    return event_loop::TimeEvent::kNoMore;
  }
  // Only `always` stops the server on an AOF error.
  if (aof_ != nullptr && !aof_->Cron(db_.get())) {
    Stop();
    return event_loop::TimeEvent::kNoMore;
  }
  ActiveExpireCycle(db_.get());
  return kCronIntervalMs;
}
}  // namespace redis_simple
