#include "server/io_thread.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "connection/connection.h"
#include "event_loop/file_event.h"
#include "event_loop/loop.h"
#include "event_loop/time_event.h"
#include "logging/logger.h"
#include "server/client.h"
#include "server/client_connection/write_reply.h"

namespace redis_simple {
namespace {
bool SetNonBlockingCloseOnExec(int fd) {
  const int status_flags = fcntl(fd, F_GETFL);
  return status_flags >= 0 &&
         fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0 &&
         fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

event_loop::CallbackStatus OnWakeup(event_loop::Loop* /*loop*/, int /*fd*/,
                                    std::function<void()>* const handler,
                                    int /*mask*/) {
  (*handler)();
  return event_loop::CallbackStatus::kOk;
}

// Registers `handler` for when `wakeup` is signalled. The handler must
// outlive the registration.
bool Watch(event_loop::Loop* const loop, const Wakeup& wakeup,
           std::function<void()>* const handler) {
  return loop->CreateFileEvent(
             wakeup.ReadFd(),
             event_loop::FileEvent::Create(
                 OnWakeup, nullptr, handler,
                 event_loop::ToInt(event_loop::EventFlag::kReadable))) ==
         event_loop::Status::kOk;
}
}  // namespace

std::unique_ptr<Wakeup> Wakeup::Create() {
  std::array<int, 2> fds{};
  if (pipe(fds.data()) < 0) {
    return nullptr;
  }
  auto wakeup = std::unique_ptr<Wakeup>(new Wakeup(fds[0], fds[1]));
  if (!SetNonBlockingCloseOnExec(fds[0]) ||
      !SetNonBlockingCloseOnExec(fds[1])) {
    return nullptr;
  }
  return wakeup;
}

Wakeup::~Wakeup() {
  close(read_fd_);
  close(write_fd_);
}

void Wakeup::Signal() const {
  const char byte = 1;
  while (true) {
    // Retry only when a signal interrupts the write. Any other failure is
    // EAGAIN from a full pipe, which is already readable, so it loses nothing.
    if (write(write_fd_, &byte, 1) >= 0 || errno != EINTR) {
      return;
    }
  }
}

void Wakeup::Drain() const {
  std::array<char, 64> bytes{};
  while (true) {
    const ssize_t result = read(read_fd_, bytes.data(), bytes.size());
    // A read that does not fill the buffer emptied the pipe, so the usual
    // single pending byte costs one call, not a second that finds nothing.
    // Keep reading after a full buffer or a signal interruption.
    if (std::cmp_less(result, bytes.size()) &&
        (result >= 0 || errno != EINTR)) {
      return;
    }
  }
}

std::unique_ptr<IoThread> IoThread::Create(
    size_t index, event_loop::Loop* const main_loop, ReadyCallback on_ready,
    const std::atomic<bool>* const main_running) {
  std::unique_ptr<event_loop::Loop> loop = event_loop::Loop::Create();
  std::unique_ptr<Wakeup> inbox_wakeup = Wakeup::Create();
  std::unique_ptr<Wakeup> outbox_wakeup = Wakeup::Create();
  if (main_loop == nullptr || loop == nullptr || inbox_wakeup == nullptr ||
      outbox_wakeup == nullptr) {
    return nullptr;
  }
  auto thread = std::unique_ptr<IoThread>(new IoThread(
      index, std::move(loop), std::move(inbox_wakeup), std::move(outbox_wakeup),
      main_loop, std::move(on_ready), main_running));
  // The I/O thread's loop is set up before the thread starts, which orders
  // these writes before its first access.
  if (!Watch(thread->loop_.get(), *thread->inbox_wakeup_,
             &thread->inbox_handler_) ||
      !Watch(main_loop, *thread->outbox_wakeup_, &thread->outbox_handler_)) {
    return nullptr;
  }
  thread->outbox_watched_ = true;
  thread->loop_->SetBeforeSleep(
      [instance = thread.get()] { instance->BeforeSleep(); });
  thread->loop_->SetAfterSleep(
      [instance = thread.get()] { instance->running_.store(true); });
  thread->loop_->CreateTimeEvent(event_loop::TimeEvent::Create(
      [instance = thread.get()](int64_t /*id*/) {
        instance->idle_client_sweeper_.Sweep();
        return IdleClientSweeper::kIntervalMs;
      },
      nullptr));
  // The project does not throw, but std::thread reports a failure to start
  // only by throwing std::system_error; catch it at this boundary so the
  // caller sees an ordinary error.
  try {
    thread->thread_ =
        std::thread([instance = thread.get()] { instance->Run(); });
  } catch (const std::system_error&) {
    return nullptr;
  }
  return thread;
}

IoThread::IoThread(size_t index, std::unique_ptr<event_loop::Loop> loop,
                   std::unique_ptr<Wakeup> inbox_wakeup,
                   std::unique_ptr<Wakeup> outbox_wakeup,
                   event_loop::Loop* const main_loop, ReadyCallback on_ready,
                   const std::atomic<bool>* const main_running)
    : index_(index),
      loop_(std::move(loop)),
      inbox_wakeup_(std::move(inbox_wakeup)),
      outbox_wakeup_(std::move(outbox_wakeup)),
      main_loop_(main_loop),
      on_ready_(std::move(on_ready)),
      inbox_handler_([this] { HandleInbox(); }),
      outbox_handler_([this] { HandleOutbox(); }),
      main_running_(main_running) {}

IoThread::~IoThread() {
  Stop();
  if (outbox_watched_) {
    main_loop_->DeleteFileEvent(outbox_wakeup_->ReadFd(),
                                event_loop::EventFlag::kReadable);
  }
}

void IoThread::Assign(Client* const client) {
  ++clients_;
  Send(std::span<Client* const>(&client, 1), false);
}

void IoThread::Return(std::span<Client* const> clients) { Send(clients, true); }

void IoThread::Send(std::span<Client* const> clients, bool returning) {
  if (clients.empty()) {
    return;
  }
  bool was_empty = false;
  {
    const std::scoped_lock lock(mutex_);
    was_empty = inbox_.empty();
    for (Client* const client : clients) {
      inbox_.push_back({client, returning});
    }
  }
  // The thread takes the whole inbox before it sleeps and on each wakeup,
  // so only the first message since then needs to wake it, and only if it
  // is asleep, as Redis' sendPendingClientsToIOThreads does. If it went to
  // sleep after this load, it took the inbox again after saying so
  // (BeforeSleep), and the lock orders that after the push.
  if (was_empty && !running_.load()) {
    inbox_wakeup_->Signal();
  }
}

void IoThread::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  pause_changed_.notify_all();
  inbox_wakeup_->Signal();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void IoThread::Pause() {
  if (stopped_) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  pause_requested_ = true;
  lock.unlock();
  inbox_wakeup_->Signal();
  lock.lock();
  pause_changed_.wait(lock, [this] { return paused_; });
}

void IoThread::Resume() {
  {
    const std::scoped_lock lock(mutex_);
    pause_requested_ = false;
  }
  pause_changed_.notify_all();
}

IoThreadInfo IoThread::Info() const {
  return {clients_, reads_processed_.load(std::memory_order_relaxed),
          writes_processed_.load(std::memory_order_relaxed)};
}

void IoThread::Run() { loop_->Run(); }

void IoThread::HandleInbox() {
  inbox_wakeup_->Drain();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitWhilePaused(&lock);
    if (stopping_) {
      loop_->Stop();
      return;
    }
  }
  ProcessInbox();
}

void IoThread::BeforeSleep() {
  // Messages sent while this thread was awake came without a wakeup, as in
  // Redis' IOThreadBeforeSleep. If there were some, more may follow soon,
  // so poll without waiting and stay awake; the main thread then keeps
  // sending without wakeups.
  const bool took_messages = ProcessInbox();
  if (!took_messages) {
    running_.store(false);
    // A message sent before Send could see the store came without a wakeup.
    ProcessInbox();
  }
  loop_->SetDontWait(took_messages);
}

bool IoThread::ProcessInbox() {
  {
    const std::scoped_lock lock(mutex_);
    // Stopping leaves the inbox to the main thread, as HandleInbox does.
    if (stopping_) {
      return false;
    }
    incoming_.swap(inbox_);
  }
  if (incoming_.empty()) {
    return false;
  }
  for (const Message& message : incoming_) {
    if (message.returning) {
      // Its commands ran: send the replies, then read again unless the
      // output is too far behind.
      message.client->SetHandedToMain(false);
      message.client->SetReadsPaused(message.client->ShouldPauseReads());
      WriteReplies(message.client);
    } else {
      idle_client_sweeper_.Add(message.client);
      StartReading(message.client);
    }
  }
  incoming_.clear();
  return true;
}

void IoThread::WaitWhilePaused(std::unique_lock<std::mutex>* const lock) {
  while (pause_requested_ && !stopping_) {
    paused_ = true;
    pause_changed_.notify_all();
    pause_changed_.wait(*lock);
  }
  paused_ = false;
}

void IoThread::StartReading(Client* const client) {
  if (!client->Connection()->SetReadCallback(
          [this, client](connection::Connection* /*connection*/) {
            OnReadable(client);
          })) {
    RS_LOG_DEBUG("failed to set the read callback\n");
    Close(client);
    HandToMain(client, true);
  }
}

void IoThread::OnReadable(Client* const client) {
  if (client->HandedToMain() || client->ReadsPaused()) {
    // More input arrived while the main thread runs this client's commands,
    // or while its output is too far behind. Stop watching the socket, which
    // stays readable, until WriteReplies starts reading again. Most clients
    // wait for their replies, so this is rare, and the socket otherwise stays
    // registered across handoffs, as in Redis 8.
    client->Connection()->UnsetReadCallback();
    return;
  }
  const ssize_t nread = client->ReadQuery();
  reads_processed_.fetch_add(1, std::memory_order_relaxed);
  if (nread <= 0) {
    if (client->Connection()->State() !=
        connection::ConnectionState::kConnected) {
      Close(client);
      HandToMain(client, true);
    }
    return;
  }
  if (client->ParseQueries()) {
    HandToMain(client, false);
  }
}

void IoThread::WriteReplies(Client* const client) {
  connection::Connection* const connection = client->Connection();
  const ssize_t written = client_connection::WritePendingReplies(client);
  writes_processed_.fetch_add(1, std::memory_order_relaxed);
  if (written < 0 &&
      connection->State() != connection::ConnectionState::kConnected) {
    Close(client);
    HandToMain(client, true);
    return;
  }
  if (client->HasPendingReplies()) {
    if (!connection->HasWriteCallback() &&
        !connection->SetWriteCallback(
            [this, client](connection::Connection* /*connection*/) {
              WriteReplies(client);
            })) {
      RS_LOG_DEBUG("failed to install the write callback\n");
    }
  } else {
    if (connection->HasWriteCallback() && !connection->UnsetWriteCallback()) {
      RS_LOG_DEBUG("failed to unset the write callback\n");
    }
    if (client->ShouldCloseAfterReply()) {
      Close(client);
      HandToMain(client, true);
      return;
    }
  }
  if (client->ReadsPaused() && client->ShouldResumeReads()) {
    client->SetReadsPaused(false);
  }
  if (!client->ReadsPaused() && !connection->HasReadCallback()) {
    StartReading(client);
  }
}

void IoThread::HandToMain(Client* const client, bool closed) {
  if (closed) {
    // The main thread may free the client as soon as it sees it.
    idle_client_sweeper_.Remove(client);
  } else {
    // The main thread owns the client until it returns it. The read callback
    // stays registered, saving two epoll_ctl calls per request; OnReadable
    // leaves the client alone meanwhile. The write callback goes, as only
    // WriteReplies sends replies, once the client is back.
    client->SetHandedToMain(true);
    connection::Connection* const connection = client->Connection();
    if (connection->HasWriteCallback()) {
      connection->UnsetWriteCallback();
    }
  }
  // Hand clients over only after this iteration's events: the connection's
  // event callback reads its state after the client callback returns, and
  // the main thread may free a closed client as soon as it sees it. One
  // batch per iteration also takes the lock and wakes the main thread once.
  handoffs_.push_back({client, closed});
  if (!handoffs_scheduled_) {
    handoffs_scheduled_ = true;
    loop_->Defer([this] { SendHandoffs(); });
  }
}

void IoThread::SendHandoffs() {
  handoffs_scheduled_ = false;
  bool was_empty = false;
  {
    const std::scoped_lock lock(mutex_);
    was_empty = outbox_.empty();
    outbox_.insert(outbox_.end(), handoffs_.begin(), handoffs_.end());
  }
  handoffs_.clear();
  // As Send does: the main thread takes the outbox before it sleeps
  // (TakeHandoffs), so it needs a wakeup only if it is asleep.
  if (was_empty && (main_running_ == nullptr || !main_running_->load())) {
    outbox_wakeup_->Signal();
  }
}

void IoThread::Close(Client* const client) {
  // Unregisters the socket from this thread's loop, so freeing the client on
  // the main thread touches no loop.
  client->Connection()->Close();
}

void IoThread::HandleOutbox() {
  outbox_wakeup_->Drain();
  TakeHandoffs();
}

bool IoThread::TakeHandoffs() {
  {
    const std::scoped_lock lock(mutex_);
    ready_.swap(outbox_);
  }
  if (ready_.empty()) {
    return false;
  }
  on_ready_(this, &ready_);
  ready_.clear();
  return true;
}
}  // namespace redis_simple
