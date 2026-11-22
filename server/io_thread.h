#ifndef REDIS_SIMPLE_SERVER_IO_THREAD_H_
#define REDIS_SIMPLE_SERVER_IO_THREAD_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "event_loop/loop.h"
#include "server/idle_client_sweeper.h"
#include "utils/cache_line.h"

namespace redis_simple {
class Client;

// A pipe that wakes an event loop; signalling it is safe from any thread.
class Wakeup {
 public:
  static std::unique_ptr<Wakeup> Create();
  Wakeup(const Wakeup&) = delete;
  Wakeup& operator=(const Wakeup&) = delete;
  Wakeup(Wakeup&&) = delete;
  Wakeup& operator=(Wakeup&&) = delete;
  ~Wakeup();

  int ReadFd() const { return read_fd_; }
  // Makes ReadFd() readable until Drain().
  void Signal() const;
  void Drain() const;

 private:
  Wakeup(int read_fd, int write_fd) : read_fd_(read_fd), write_fd_(write_fd) {}
  int read_fd_;
  int write_fd_;
};

// Whether a thread is awake, not waiting in its poll, as Redis' `running`
// flags: a thread that sends to it signals its wakeup only when it is not,
// since the thread takes its queue before it sleeps. That is safe only with
// this order on both sides. The sender queues, then reads the flag. The
// thread clears the flag, then takes its queue once more (SleepAndTake), so
// what a sender queued without a wakeup is taken before the thread waits.
//
// The flag has cache lines of its own. Senders read it for every batch, and
// beside data its thread keeps writing each of those reads would miss.
class alignas(utils::kCacheLineBytes) RunningFlag {
 public:
  // Any thread.
  [[nodiscard]] bool Running() const { return running_.load(); }
  // The thread itself, when its poll returns.
  void SetRunning() { running_.store(true); }
  // The thread itself, before a poll that may wait and after it took its
  // queue and found nothing: clears the flag, then runs `take_queue`, which
  // takes the queue again, and returns what that returns: whether the queue
  // held anything.
  template <typename TakeQueue>
  bool SleepAndTake(TakeQueue&& take_queue) {
    running_.store(false);
    return std::forward<TakeQueue>(take_queue)();
  }

 private:
  std::atomic<bool> running_{true};
};
static_assert(alignof(RunningFlag) == utils::kCacheLineBytes &&
              sizeof(RunningFlag) == utils::kCacheLineBytes);

// Counters for INFO, like Redis' per-thread "io_thread_<n>" lines.
struct IoThreadInfo {
  size_t clients{};
  uint64_t reads_processed{};
  uint64_t writes_processed{};
};

// One I/O thread, as in Redis 8: an event loop on its own thread that owns
// the sockets of the clients assigned to it. It reads a client's requests
// and frames them, then hands the client to the main thread, which runs the
// commands and hands it back; the I/O thread then writes the replies and
// resumes reading. A client belongs to exactly one side at a time, and the
// queues between them order every access, so neither touches a client the
// other holds.
//
// A connection closed on the I/O thread (by the peer, an error, or after a
// reply that ends it) is closed there and handed to the main thread, which
// frees the client.
class IoThread {
 public:
  // A client handed to the main thread.
  struct Handoff {
    Client* client;
    // The connection is closed; the main thread only frees the client.
    bool closed;
  };
  // Runs on the main thread with the clients this thread handed over.
  using ReadyCallback = std::function<void(IoThread*, std::vector<Handoff>*)>;

  // Starts the thread; `main_loop` runs `on_ready`. Returns nullptr if the
  // thread or its event loop cannot be created. `main_running`, if given,
  // is the main thread's flag; that thread then calls TakeHandoffs before
  // it sleeps, so a handoff made while it is awake needs no wakeup.
  // `cron_interval_ms` is how often the thread's timer runs, which sweeps
  // its idle clients; a test lengthens it so that only a wakeup ends the
  // thread's poll.
  static std::unique_ptr<IoThread> Create(
      size_t index, event_loop::Loop* main_loop, ReadyCallback on_ready,
      const RunningFlag* main_running = nullptr,
      int64_t cron_interval_ms = IdleClientSweeper::kIntervalMs);
  IoThread(const IoThread&) = delete;
  IoThread& operator=(const IoThread&) = delete;
  IoThread(IoThread&&) = delete;
  IoThread& operator=(IoThread&&) = delete;
  // Stops the thread; its event loop lives on until destruction so clients
  // can still close their connections single-threaded.
  ~IoThread();

  size_t Index() const { return index_; }
  // The loop clients of this thread register their sockets with.
  event_loop::Loop* Loop() { return loop_.get(); }

  // Main thread. Assign starts reading a new client; Return hands back
  // clients whose commands ran, to write their replies and resume reading.
  // Return takes the queue's lock and wakes the thread at most once for all
  // of them, as Redis' sendPendingClientsToIOThreads sends each thread its
  // clients as one list.
  void Assign(Client* client);
  void Return(std::span<Client* const> clients);
  // Main thread: runs `on_ready` with the clients this thread handed over,
  // if any, and returns whether there were. With `main_running`, the main
  // thread calls it before it sleeps, as Redis' beforeSleep calls
  // processClientsOfAllIOThreads.
  bool TakeHandoffs();
  // Stops the thread for good; the main thread then owns every client.
  void Stop();
  // Waits until the thread is idle between events, and keeps it there until
  // Resume, for work such as fork() that must not race the thread's
  // allocations.
  void Pause();
  void Resume();
  // Clients assigned and not yet freed; maintained by the main thread.
  size_t ClientCount() const { return clients_; }
  void ClientFreed() { --clients_; }
  IoThreadInfo Info() const;

 private:
  struct Message {
    Client* client;
    bool returning;
  };

  IoThread(size_t index, std::unique_ptr<event_loop::Loop> loop,
           std::unique_ptr<Wakeup> inbox_wakeup,
           std::unique_ptr<Wakeup> outbox_wakeup, event_loop::Loop* main_loop,
           ReadyCallback on_ready, const RunningFlag* main_running);
  void Send(std::span<Client* const> clients, bool returning);

  // I/O thread.
  void Run();
  void HandleInbox();
  // Handles the messages the main thread sent; returns whether there were.
  bool ProcessInbox();
  void BeforeSleep();
  void WaitWhilePaused(std::unique_lock<std::mutex>* lock);
  void StartReading(Client* client);
  void OnReadable(Client* client);
  void WriteReplies(Client* client);
  void HandToMain(Client* client, bool closed);
  void SendHandoffs();
  static void Close(Client* client);

  // Main thread.
  void HandleOutbox();

  size_t index_;
  std::unique_ptr<event_loop::Loop> loop_;
  std::unique_ptr<Wakeup> inbox_wakeup_;
  std::unique_ptr<Wakeup> outbox_wakeup_;
  event_loop::Loop* main_loop_;
  ReadyCallback on_ready_;
  // Registered with the two loops; they run HandleInbox and HandleOutbox.
  std::function<void()> inbox_handler_;
  std::function<void()> outbox_handler_;
  bool outbox_watched_{false};
  std::thread thread_;

  // Guarded by `mutex_`.
  std::mutex mutex_;
  std::condition_variable pause_changed_;
  std::vector<Message> inbox_;
  std::vector<Handoff> outbox_;
  bool stopping_{false};
  bool pause_requested_{false};
  bool paused_{false};

  // I/O thread only: the inbox being handled, and the clients to hand over
  // once the current iteration's events are done.
  std::vector<Message> incoming_;
  std::vector<Handoff> handoffs_;
  bool handoffs_scheduled_{false};
  // I/O thread only: the clients assigned to this thread and not closed.
  // A time event on this thread's loop sweeps them.
  IdleClientSweeper idle_client_sweeper_;
  // Main thread only.
  std::vector<Handoff> ready_;
  size_t clients_{};
  bool stopped_{false};

  std::atomic<uint64_t> reads_processed_{0};
  std::atomic<uint64_t> writes_processed_{0};
  // This thread's flag, as Redis' IOThread.running: Send wakes it only
  // when it is not awake, since it takes its inbox before it sleeps. The
  // main thread's flag, when given, does the same for SendHandoffs.
  RunningFlag running_;
  const RunningFlag* main_running_;
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_IO_THREAD_H_
