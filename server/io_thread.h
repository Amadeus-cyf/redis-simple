#ifndef REDIS_SIMPLE_SERVER_IO_THREAD_H_
#define REDIS_SIMPLE_SERVER_IO_THREAD_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "event_loop/loop.h"

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
  // thread or its event loop cannot be created.
  static std::unique_ptr<IoThread> Create(size_t index,
                                          event_loop::Loop* main_loop,
                                          ReadyCallback on_ready);
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

  // Main thread. Assign starts reading a new client; Return hands back a
  // client whose commands ran, to write its replies and resume reading.
  void Assign(Client* client);
  void Return(Client* client);
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
           ReadyCallback on_ready);
  void Send(Message message);

  // I/O thread.
  void Run();
  void HandleInbox();
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
  // Main thread only.
  std::vector<Handoff> ready_;
  size_t clients_{};
  bool stopped_{false};

  std::atomic<uint64_t> reads_processed_{0};
  std::atomic<uint64_t> writes_processed_{0};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_IO_THREAD_H_
