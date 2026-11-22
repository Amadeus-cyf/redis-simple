#ifndef REDIS_SIMPLE_SERVER_SERVER_H_
#define REDIS_SIMPLE_SERVER_SERVER_H_

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "event_loop/loop.h"
#include "server/active_expire.h"
#include "server/aof/aof.h"
#include "server/client.h"
#include "server/idle_client_sweeper.h"
#include "server/io_thread.h"
#include "server/rdb/rdb.h"
#include "server/server_context.h"
#include "server/server_options.h"

namespace redis_simple {
// The server: the main thread's event loop, the database and its
// persistence, the listening socket, and every client. Commands run on the
// thread that calls Run().
class Server {
 public:
  static std::unique_ptr<Server> Create();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  // Loads the data, then serves clients until Stop() or a stop signal.
  // Returns false if the server could not start.
  bool Run(const ServerOptions& options);
  // Makes Run() return after the current event-loop iteration.
  void Stop();
  event_loop::Loop* Loop() { return loop_.get(); }
  db::RedisDb* Db() { return db_.get(); }
  aof::Aof* Aof() { return aof_.get(); }
  // Password every new client must AUTH with; empty when auth is disabled.
  std::string_view RequirePass() const { return require_pass_; }
  // What clients' commands reach of the server; valid while Run() runs.
  const ServerContext* Context() const { return &context_; }
  bool ProtectedMode() const { return protected_mode_; }
  void AddClient(std::unique_ptr<Client> client) {
    Client* const key = client.get();
    clients_.emplace(key, std::move(client));
    // With threaded I/O the client's I/O thread sweeps it instead.
    if (io_threads_.empty()) {
      idle_client_sweeper_.Add(key);
    }
  }
  // Closes the client's connection now and destroys the client once the
  // current event has been handled. Returns false only for a null client.
  bool RemoveClient(Client* client);
  // Keeps the replies to the commands `client` just ran until the event loop
  // is about to wait for events again. By then every client that was
  // readable has run its commands, so one AOF flush covers them all, and the
  // replies are sent after it.
  void HoldReplies(Client* client);
  // Threaded I/O, enabled when --io-threads is above 1, as in Redis 8: each
  // client belongs to one I/O thread, which reads and frames its requests and
  // writes its replies, while commands run here on the main thread. Returns
  // the thread a new client should use (the one with the fewest clients), or
  // null when I/O is single-threaded.
  IoThread* NextIoThread();
  const std::vector<std::unique_ptr<IoThread>>& IoThreads() const {
    return io_threads_;
  }
  // Pauses every I/O thread around work such as fork() that must not race
  // their allocations.
  void PauseIoThreads();
  void ResumeIoThreads();
  ~Server() = default;

 private:
  Server();
  // The steps of Run(). LoadData: enters the working directory, loads the
  // RDB file or opens and replays the AOF. StartServing: listens on
  // `listener`, starts the I/O threads, and accepts connections. Both
  // return false after a failure, which Run() cleans up after.
  bool LoadData(const ServerOptions& options);
  bool StartServing(const ServerOptions& options,
                    connection::Connection* listener);
  bool InstallAcceptCallback();
  int64_t ServerCron();
  bool StartIoThreads(size_t thread_count);
  void StopIoThreads();
  // The clients of one handoff whose commands ran, as a run of `size`
  // entries of `returning_clients_`, and the I/O thread that gets them back
  // to write the replies.
  struct ReturningBatch {
    IoThread* thread;
    size_t size;
  };
  // Runs the commands of the clients an I/O thread handed over. They go
  // back to it, to write the replies, after the AOF flush in BeforeSleep,
  // or at once when there is no AOF.
  void RunHandedOverClients(IoThread* thread,
                            std::vector<IoThread::Handoff>* handoffs);
  // Runs each time the event loop is about to wait for events: runs the
  // clients the I/O threads handed over, flushes the AOF once for every
  // command run since the last time, then lets the replies to those
  // commands go out.
  void BeforeSleep();
  // Runs every I/O thread's handed-over clients; returns whether there were.
  bool TakeIoThreadHandoffs();
  void ReleaseHeldReplies();
  void ReturnClientsToIoThreads();
  int fd_{-1};
  std::unique_ptr<event_loop::Loop> loop_;
  std::unordered_map<Client*, std::unique_ptr<Client>> clients_;
  // The clients this thread reads and writes: all of them without threaded
  // I/O, none with it.
  IdleClientSweeper idle_client_sweeper_;
  std::unique_ptr<db::RedisDb> db_;
  std::unique_ptr<aof::Aof> aof_;
  std::unique_ptr<rdb::Rdb> rdb_;
  ForkStats fork_stats_;
  ActiveExpire active_expire_;
  // When the fork hooks last let a fork start.
  int64_t fork_start_microseconds_{};
  std::string require_pass_;
  bool protected_mode_{true};
  // This thread's flag, as Redis' server.running: the I/O threads wake it
  // only when it is not awake, since BeforeSleep takes their handoffs.
  // Declared before io_threads_, which read it, so it outlives them.
  RunningFlag main_running_;
  std::vector<std::unique_ptr<IoThread>> io_threads_;
  ServerContext context_;
  // Clients whose replies wait for the AOF flush in BeforeSleep: those the
  // main thread writes to, and those an I/O thread writes to.
  std::vector<Client*> held_clients_;
  std::vector<Client*> returning_clients_;
  std::vector<ReturningBatch> returning_batches_;
  // The held clients being replied to, reused between iterations.
  std::vector<Client*> releasing_clients_;
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_SERVER_H_
