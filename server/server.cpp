#include "server/server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "containers/dict.h"
#include "event_loop/file_event.h"
#include "event_loop/time_event.h"
#include "logging/logger.h"
#include "server/active_expire.h"
#include "server/client_connection/client_connection.h"
#include "server/client_connection/read_query.h"
#include "server/client_connection/write_reply.h"
#include "server/db/db.h"
#include "server/shutdown.h"
#include "utils/time.h"

namespace redis_simple {
Server::Server(const MemoryPurger::PurgeFunction purge_memory)
    : loop_(event_loop::Loop::Create()),
      memory_purge_{.purge = purge_memory},
      memory_purger_(&memory_purge_),
      db_(db::RedisDb::Create()) {}

std::unique_ptr<Server> Server::Create(
    const MemoryPurger::PurgeFunction purge_memory) {
  return std::unique_ptr<Server>(new Server(purge_memory));
}

bool Server::Run(const ServerOptions& options) {
  if (loop_ == nullptr || options.bind_address.empty() || options.port <= 0 ||
      options.port > 65535 || !shutdown::InstallSignalHandlers()) {
    return false;
  }
  aof_.reset();
  require_pass_ = options.require_pass;
  protected_mode_ = options.protected_mode;
  if (!LoadData(options)) {
    return false;
  }
  // Closes the listening socket when Run returns.
  connection::Connection listener(
      connection::Context{.loop = loop_.get(), .fd = -1});
  if (!StartServing(options, &listener)) {
    fd_ = -1;
    // Also closes the AOF, which stops its background thread.
    StopIoThreads();
    return false;
  }
  context_ = {.db = db_.get(),
              .aof = aof_.get(),
              .rdb = rdb_.get(),
              .changes = rdb_->ChangeCounter(),
              .required_password = require_pass_,
              .io_threads = io_threads_,
              .forks = &fork_stats_};
  loop_->CreateTimeEvent(event_loop::TimeEvent::Create(
      [this](int64_t /*id*/) { return ServerCron(); }, nullptr));
  loop_->SetBeforeSleep([this] { BeforeSleep(); });
  loop_->Run();
  loop_->DeleteFileEvent(fd_, event_loop::EventFlag::kReadable);
  fd_ = -1;
  StopIoThreads();
  return true;
}

bool Server::LoadData(const ServerOptions& options) {
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
  // threads, as they read and parse.
  // The hash tables follow Redis' updateDictResizePolicy: the child, which
  // inherits the setting made before the fork, never rehashes, and the
  // parent avoids it until the cron sees the child gone.
  const auto before_fork = [this] {
    PauseIoThreads();
    containers::SetDictResizePolicy(containers::DictResizePolicy::kForbidden);
    // Timed from here, right before fork(), as Redis' redisFork times it.
    fork_start_microseconds_ = utils::MonotonicMicroseconds();
  };
  const auto after_fork = [this](bool forked) {
    if (forked) {
      ++fork_stats_.total_forks;
      fork_stats_.latest_fork_usec =
          utils::MonotonicMicroseconds() - fork_start_microseconds_;
    }
    containers::SetDictResizePolicy(containers::DictResizePolicy::kAvoid);
    ResumeIoThreads();
  };
  rdb_->SetForkHooks(before_fork, after_fork);
  if (!options.append_only) {
    return true;
  }
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
  return true;
}

bool Server::StartServing(const ServerOptions& options,
                          connection::Connection* const listener) {
  const connection::AddressInfo address(options.bind_address, options.port);
  if (listener->BindAndListen(address) ==
      connection::ConnectionStatus::kError) {
    return false;
  }
  fd_ = listener->Descriptor();
  // Like Redis, --io-threads counts the main thread, which runs commands
  // while the others own the client sockets.
  if (options.io_threads > 1 &&
      !StartIoThreads(static_cast<size_t>(options.io_threads) - 1)) {
    RS_LOG_WARN("failed to start %d I/O threads\n", options.io_threads - 1);
    return false;
  }
  return InstallAcceptCallback();
}

bool Server::StartIoThreads(size_t thread_count) {
  for (size_t index = 1; index <= thread_count; ++index) {
    std::unique_ptr<IoThread> thread = IoThread::Create(
        index, loop_.get(),
        [this](IoThread* const io_thread,
               std::vector<IoThread::Handoff>* const handoffs) {
          RunHandedOverClients(io_thread, handoffs);
        },
        &memory_purge_);
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
  idle_client_sweeper_.Clear();
  held_clients_.clear();
  returning_clients_.clear();
  returning_batches_.clear();
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
  const size_t returning_before = returning_clients_.size();
  for (const IoThread::Handoff& handoff : *handoffs) {
    if (handoff.closed) {
      thread->ClientFreed();
      RemoveClient(handoff.client);
      continue;
    }
    handoff.client->RunParsedQueries();
    returning_clients_.push_back(handoff.client);
  }
  if (const size_t size = returning_clients_.size() - returning_before;
      size > 0) {
    returning_batches_.push_back({thread, size});
  }
  // Without an AOF no flush has to come first, and an I/O thread that gets
  // its clients back now writes their replies while the main thread runs
  // the next handoff.
  if (aof_ == nullptr) {
    ReturnClientsToIoThreads();
  }
}

void Server::HoldReplies(Client* const client) {
  if (!client->AwaitsFlush()) {
    client->SetAwaitsFlush(true);
    held_clients_.push_back(client);
  }
}

void Server::BeforeSleep() {
  active_expire_.RunFastCycle(db_.get());
  // One write, and one fsync with `always`, persists the commands of every
  // client that ran in this iteration, as Redis' beforeSleep does. Only then
  // are their replies sent.
  if (aof_ != nullptr && !aof_->Flush()) {
    // The held replies promise writes that are not on disk, so they are
    // never sent: the clients stay marked, and the server stops.
    held_clients_.clear();
    returning_clients_.clear();
    returning_batches_.clear();
    Stop();
    return;
  }
  ReleaseHeldReplies();
}

void Server::ReleaseHeldReplies() {
  // Replying can remove the client, which edits `held_clients_`.
  releasing_clients_.swap(held_clients_);
  for (Client* const client : releasing_clients_) {
    client->SetAwaitsFlush(false);
    client_connection::ReplyToQueries(this, client);
  }
  releasing_clients_.clear();
  ReturnClientsToIoThreads();
}

void Server::ReturnClientsToIoThreads() {
  size_t first = 0;
  for (const ReturningBatch& batch : returning_batches_) {
    batch.thread->Return(
        std::span(returning_clients_).subspan(first, batch.size));
    first += batch.size;
  }
  returning_clients_.clear();
  returning_batches_.clear();
}

void Server::Stop() {
  if (loop_ != nullptr) {
    loop_->Stop();
  }
}

bool Server::RemoveClient(Client* client) {
  if (client == nullptr) {
    return false;
  }
  idle_client_sweeper_.Remove(client);
  if (client->AwaitsFlush()) {
    std::erase(held_clients_, client);
  }
  client->Close();
  // Destroy the client after the current event, which may still use it.
  loop_->Defer([this, client] { clients_.erase(client); });
  return true;
}

bool Server::InstallAcceptCallback() {
  auto file_event = event_loop::FileEvent::Create(
      client_connection::AcceptConnectionCallback, nullptr, this,
      event_loop::ToInt(event_loop::EventFlag::kReadable));
  if (loop_->CreateFileEvent(fd_, std::move(file_event)) ==
      event_loop::Status::kError) {
    RS_LOG_DEBUG("failed to add the accept file event\n");
    return false;
  }
  return true;
}

int64_t Server::ServerCron() {
  constexpr int64_t kCronIntervalMs = IdleClientSweeper::kIntervalMs;
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
  const bool child_running = rdb_->BackgroundSaveInProgress() ||
                             (aof_ != nullptr && aof_->RewriteInProgress());
  containers::SetDictResizePolicy(child_running
                                      ? containers::DictResizePolicy::kAvoid
                                      : containers::DictResizePolicy::kEnabled);
  active_expire_.RunSlowCycle(db_.get());
  // A tenth of each tick, as UNLINK and FLUSHDB have already replied.
  constexpr int64_t kLazyFreeBudgetMs = 10;
  const bool freed_lazily = db_->FreeLazily(kLazyFreeBudgetMs);
  bool rehashed = false;
  if (!child_running) {
    // As in Redis' databasesCron: one millisecond per tick. Skipped while a
    // child runs, since moving entries writes to memory the child shares.
    constexpr int64_t kRehashBudgetMicroseconds = 1000;
    rehashed = db_->RehashIncrementally(kRehashBudgetMicroseconds);
  }
  // Freeing idle clients' buffers counts as work, so the memory goes back
  // to the system within a second.
  const bool released_buffers = idle_client_sweeper_.Sweep();
  const bool did_work = MainThreadDidWork(freed_lazily || rehashed);
  memory_purger_.Tick(did_work || released_buffers);
  return kCronIntervalMs;
}

bool Server::MainThreadDidWork(const bool cron_freed_memory) {
  // Expiry, the lazy free and a finished rehash release memory without any
  // event, so a change in the key count, or this tick having freed detached
  // values or moved a rehash along, counts as work too. The tick that frees
  // the last of it counts: its memory is the last to go back to the system.
  const uint64_t file_events = loop_->FileEventsPolled();
  const size_t key_count = db_->KeyCount();
  const bool did_work = cron_freed_memory || file_events != cron_file_events_ ||
                        key_count != cron_key_count_;
  cron_file_events_ = file_events;
  cron_key_count_ = key_count;
  return did_work;
}
}  // namespace redis_simple
