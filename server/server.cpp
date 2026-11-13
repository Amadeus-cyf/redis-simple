#include "server/server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "event_loop/file_event.h"
#include "event_loop/time_event.h"
#include "logging/logger.h"
#include "server/active_expire.h"
#include "server/client_connection/client_connection.h"
#include "server/client_connection/read_query.h"
#include "server/client_connection/write_reply.h"
#include "server/db/db.h"
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
  // Redis' dir: the RDB file and the AOF are relative to it.
  if (!options.dir.empty() && chdir(options.dir.c_str()) != 0) {
    RS_LOG_ERROR("Can't chdir to '%s': %s\n", options.dir.c_str(),
                 std::strerror(errno));
    return false;
  }
  db_ = db::RedisDb::Create();
  rdb_ = rdb::Rdb::Create(options.rdb_options);
  if (db_ == nullptr) {
    return false;
  }
  // Redis' loadDataFromDisk: the AOF when it is on, otherwise the RDB file.
  if (!options.append_only && !rdb_->Load(db_.get())) {
    return false;
  }
  // Threads that allocate or free memory wait out a child's fork: the I/O
  // threads, as they read and parse, and the database's thread that frees
  // UNLINKed values.
  const auto before_fork = [this] {
    PauseIoThreads();
    db_->PauseBackgroundFrees();
  };
  const auto after_fork = [this] {
    db_->ResumeBackgroundFrees();
    ResumeIoThreads();
  };
  rdb_->SetForkHooks(before_fork, after_fork);
  if (options.append_only) {
    // An RDB base file follows the RDB options, as in Redis.
    aof::Options aof_options = options.aof_options;
    aof_options.rdb_compression = options.rdb_options.compression;
    aof_options.rdb_checksum = options.rdb_options.checksum;
    aof_ = aof::Aof::Open(aof_options, db_.get());
    if (aof_ == nullptr) {
      return false;
    }
    // Redis counts the loaded commands' changes as unsaved.
    rdb_->AddChanges(aof_->ReplayedChanges());
    aof_->SetForkHooks(before_fork, after_fork);
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
  context_ = {.db = db_.get(),
              .aof = aof_.get(),
              .rdb = rdb_.get(),
              .changes = rdb_->ChangeCounter(),
              .required_password = require_pass_,
              .io_threads = io_threads_};
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
  context_ = {};
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
    // Redis' prepareForShutdown: when the final save fails, the server keeps
    // running.
    if (rdb_->SaveBeforeShutdown(db_.get())) {
      Stop();
      return event_loop::TimeEvent::kNoMore;
    }
    shutdown::ClearStopRequest();
    RS_LOG_WARN(
        "Errors trying to shut down the server. Check the logs for more "
        "information.\n");
  }
  // One child runs at a time: an AOF rewrite and a background save never
  // overlap. Only `always` stops the server on an AOF error.
  if (aof_ != nullptr &&
      !aof_->Cron(db_.get(), !rdb_->BackgroundSaveInProgress())) {
    Stop();
    return event_loop::TimeEvent::kNoMore;
  }
  rdb_->Cron(db_.get(), aof_ == nullptr || !aof_->RewriteInProgress());
  ActiveExpireCycle(db_.get());
  return kCronIntervalMs;
}
}  // namespace redis_simple
