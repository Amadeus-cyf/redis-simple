#include "connection.h"

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "event_loop/file_event.h"
#include "logging/logger.h"
#include "tcp/tcp.h"

namespace redis_simple::connection {
namespace {
class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&&) = delete;
  ScopedFd& operator=(ScopedFd&&) = delete;
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  [[nodiscard]] int Get() const { return fd_; }
  int Release() { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

int BindAndConnectTcp(const AddressInfo& remote,
                      const std::optional<AddressInfo>& local) {
  const tcp::TcpAddrInfo remote_addr(remote.ip, remote.port);
  const auto local_addr =
      local.has_value()
          ? std::make_optional<tcp::TcpAddrInfo>(local->ip, local->port)
          : std::nullopt;
  return tcp::TcpBindAndConnect(remote_addr, local_addr);
}

bool WouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }

size_t MaxIovCount() {
  static const size_t max_iov_count = [] {
    const long system_limit = sysconf(_SC_IOV_MAX);
    const size_t limit =
        system_limit > 0 ? static_cast<size_t>(system_limit) : 1024;
    return std::min(limit,
                    static_cast<size_t>(std::numeric_limits<int>::max()));
  }();
  return max_iov_count;
}
}  // namespace

Connection::Connection(const Context& ctx)
    : fd_(ctx.fd),
      state_(ConnectionState::kConnect),
      loop_(ctx.loop),
      read_callback_(nullptr),
      write_callback_(nullptr) {}

Connection::~Connection() { Close(); }

void Connection::Close() {
  if (fd_ < 0) {
    return;
  }
  if (loop_ != nullptr) {
    const int event_mask =
        flags_ & (event_loop::ToInt(event_loop::EventFlag::kReadable) |
                  event_loop::ToInt(event_loop::EventFlag::kWritable));
    if (event_mask != 0) {
      loop_->DeleteFileEvent(fd_, event_mask);
    }
  }
  read_callback_ = nullptr;
  write_callback_ = nullptr;
  flags_ = 0;
  close(fd_);
  fd_ = -1;
  state_ = ConnectionState::kClosed;
}

ConnectionStatus Connection::BindAndConnect(
    const AddressInfo& remote, const std::optional<AddressInfo>& local) {
  if (auto* loop = loop_) {
    ScopedFd fd(BindAndConnectTcp(remote, local));
    if (fd.Get() < 0) {
      return ConnectionStatus::kError;
    }
    fd_ = fd.Get();
    state_ = ConnectionState::kConnecting;
    auto event = event_loop::FileEvent::Create(
        nullptr, WritableEventCallback, this,
        event_loop::ToInt(event_loop::EventFlag::kWritable));
    if (loop->CreateFileEvent(fd_, std::move(event)) ==
        event_loop::Status::kError) {
      RS_LOG_DEBUG("adding connection socket event callback error");
      fd_ = -1;
      return ConnectionStatus::kError;
    }
    flags_ |= event_loop::EventFlag::kWritable;
    fd.Release();
  } else {
    RS_LOG_DEBUG("no event loop\n");
    return ConnectionStatus::kError;
  }
  return ConnectionStatus::kOk;
}

ConnectionStatus Connection::BindAndBlockingConnect(
    const AddressInfo& remote, const std::optional<AddressInfo>& local,
    int64_t timeout_ms) {
  ScopedFd fd(BindAndConnectTcp(remote, local));
  if (fd.Get() < 0) {
    return ConnectionStatus::kError;
  }
  fd_ = fd.Get();
  if (WaitWrite(timeout_ms) <= 0) {
    RS_LOG_DEBUG("wait failed\n");
    fd_ = -1;
    return ConnectionStatus::kError;
  }
  if (tcp::IsSocketError(fd_)) {
    fd_ = -1;
    return ConnectionStatus::kError;
  }
  fd.Release();
  state_ = ConnectionState::kConnected;
  return ConnectionStatus::kOk;
}

ConnectionStatus Connection::BindAndListen(const AddressInfo& addr_info) {
  ScopedFd socket(tcp::TcpCreateSocket(AF_INET, true));
  if (socket.Get() < 0) {
    RS_LOG_DEBUG("create socket failed\n");
    return ConnectionStatus::kError;
  }
  const tcp::TcpAddrInfo tcp_addr(addr_info.ip, addr_info.port);
  if (tcp::TcpBind(socket.Get(), tcp_addr) == tcp::TcpStatusCode::kError) {
    return ConnectionStatus::kError;
  }
  if (tcp::TcpListen(socket.Get()) == tcp::TcpStatusCode::kError) {
    return ConnectionStatus::kError;
  }
  fd_ = socket.Release();
  return ConnectionStatus::kOk;
}

ConnectionStatus Connection::Accept(int listener_fd, AddressInfo* addr_info) {
  if (listener_fd < 0 || fd_ >= 0 || state_ != ConnectionState::kAccepting) {
    return ConnectionStatus::kError;
  }
  tcp::TcpAddrInfo tcp_addr;
  const int fd = tcp::TcpAccept(listener_fd, &tcp_addr);
  if (fd < 0) {
    return ConnectionStatus::kError;
  }
  if (addr_info != nullptr) {
    addr_info->ip = tcp_addr.ip;
    addr_info->port = tcp_addr.port;
    RS_LOG_DEBUG("accept %s %d\n", addr_info->ip.c_str(), addr_info->port);
  }
  state_ = ConnectionState::kConnected;
  fd_ = fd;
  RS_LOG_DEBUG("conn state accept %d\n", static_cast<int>(state_));
  return ConnectionStatus::kOk;
}

bool Connection::SetReadCallback(ConnectionCallback read_callback) {
  if (!read_callback) {
    return UnsetReadCallback();
  }
  if (auto* loop = loop_) {
    auto event = event_loop::FileEvent::Create(
        ReadableEventCallback, nullptr, this,
        event_loop::ToInt(event_loop::EventFlag::kReadable));
    if (loop->CreateFileEvent(fd_, std::move(event)) ==
        event_loop::Status::kError) {
      RS_LOG_DEBUG("failed to set read callback\n");
      return false;
    }
    read_callback_ = std::move(read_callback);
    flags_ |= event_loop::EventFlag::kReadable;
  } else {
    RS_LOG_DEBUG("no event loop\n");
    return false;
  }
  return true;
}

bool Connection::UnsetReadCallback() {
  if (auto* loop = loop_) {
    if (loop->DeleteFileEvent(fd_, event_loop::EventFlag::kReadable) ==
        event_loop::Status::kError) {
      RS_LOG_DEBUG("failed to unset read callback\n");
      return false;
    }
    read_callback_ = nullptr;
    flags_ &= ~event_loop::EventFlag::kReadable;
  } else {
    RS_LOG_DEBUG("no event loop\n");
    return false;
  }
  return true;
}

bool Connection::SetWriteCallback(ConnectionCallback callback, bool barrier) {
  if (!callback) {
    return UnsetWriteCallback();
  }
  if (auto* loop = loop_) {
    int event_mask = event_loop::ToInt(event_loop::EventFlag::kWritable);
    if (barrier) {
      event_mask |= event_loop::EventFlag::kBarrier;
    }
    auto event = event_loop::FileEvent::Create(nullptr, WritableEventCallback,
                                               this, event_mask);
    if (loop->CreateFileEvent(fd_, std::move(event)) ==
        event_loop::Status::kError) {
      RS_LOG_DEBUG("failed to set write callback\n");
      return false;
    }
    write_callback_ = std::move(callback);
    flags_ |= event_loop::EventFlag::kWritable;
    if (barrier) {
      flags_ |= event_loop::EventFlag::kBarrier;
    } else {
      flags_ &= ~event_loop::EventFlag::kBarrier;
    }
  } else {
    RS_LOG_DEBUG("no event loop\n");
    return false;
  }
  return true;
}

bool Connection::UnsetWriteCallback() {
  if (auto* loop = loop_) {
    if (loop->DeleteFileEvent(fd_, event_loop::EventFlag::kWritable) ==
        event_loop::Status::kError) {
      RS_LOG_DEBUG("failed to unset write callback\n");
      return false;
    }
    write_callback_ = nullptr;
    flags_ &= ~event_loop::EventFlag::kWritable;
    flags_ &= ~event_loop::EventFlag::kBarrier;
  } else {
    RS_LOG_DEBUG("no event loop\n");
    return false;
  }
  return true;
}

ssize_t Connection::Read(char* const buffer, size_t readlen) {
  ssize_t r = -1;
  r = read(fd_, buffer, readlen);
  while (r < 0 && errno == EINTR) {
    r = read(fd_, buffer, readlen);
  }
  if (r < 0 && !WouldBlock(errno)) {
    if (state_ == ConnectionState::kConnected) {
      state_ = ConnectionState::kError;
    }
    return -1;
  }
  if (r == 0) {
    state_ = ConnectionState::kClosed;
  }
  return r;
}

ssize_t Connection::SyncRead(char* buffer, size_t readlen, int64_t timeout_ms) {
  // Try the syscall first so already-ready sockets avoid an extra poll().
  ssize_t r = -1;
  r = read(fd_, buffer, readlen);
  while (r < 0 && errno == EINTR) {
    r = read(fd_, buffer, readlen);
  }
  if (r >= 0) {
    return r;
  }
  if (!WouldBlock(errno)) {
    return -1;
  }
  if (WaitRead(timeout_ms) < 0) {
    return -1;
  }
  return Read(buffer, readlen);
}

ssize_t Connection::Write(const char* buffer, size_t len) {
  ssize_t n = -1;
  n = write(fd_, buffer, len);
  while (n < 0 && errno == EINTR) {
    n = write(fd_, buffer, len);
  }
  if (n < 0 && !WouldBlock(errno)) {
    if (state_ == ConnectionState::kConnected) {
      state_ = ConnectionState::kError;
    }
    RS_LOG_DEBUG("write failed\n");
    return -1;
  }
  return n;
}

ssize_t Connection::SyncWrite(const char* buffer, size_t len,
                              int64_t timeout_ms) const {
  size_t total_written = 0;
  while (total_written < len) {
    const ssize_t written =
        write(fd_, buffer + total_written, len - total_written);
    if (written > 0) {
      total_written += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
        errno != EINTR) {
      return -1;
    }
    if (WaitWrite(timeout_ms) < 0) {
      RS_LOG_DEBUG("wait failed\n");
      return -1;
    }
  }
  if (total_written >
      static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
    return -1;
  }
  return static_cast<ssize_t>(total_written);
}

ssize_t Connection::WriteVector(const std::vector<iovec>& blocks) {
  if (blocks.empty()) {
    return 0;
  }
  const auto block_count =
      static_cast<int>(std::min(blocks.size(), MaxIovCount()));
  ssize_t n = -1;
  n = writev(fd_, blocks.data(), block_count);
  while (n < 0 && errno == EINTR) {
    n = writev(fd_, blocks.data(), block_count);
  }
  if (n < 0 && !WouldBlock(errno)) {
    if (state_ == ConnectionState::kConnected) {
      state_ = ConnectionState::kError;
    }
    RS_LOG_DEBUG("write failed\n");
    return -1;
  }
  return n;
}

event_loop::CallbackStatus Connection::ReadableEventCallback(
    event_loop::Loop* /*loop*/, int /*fd*/, Connection* conn, int mask) {
  if (conn == nullptr || (mask & event_loop::EventFlag::kReadable) == 0) {
    return event_loop::CallbackStatus::kError;
  }
  if (conn->read_callback_) {
    conn->read_callback_(conn);
  }
  return conn->State() == ConnectionState::kClosed ||
                 conn->State() == ConnectionState::kError
             ? event_loop::CallbackStatus::kError
             : event_loop::CallbackStatus::kOk;
}

event_loop::CallbackStatus Connection::WritableEventCallback(
    event_loop::Loop* loop, int fd, Connection* conn, int mask) {
  if (conn == nullptr || (mask & event_loop::EventFlag::kWritable) == 0) {
    return event_loop::CallbackStatus::kError;
  }
  if (conn->State() == ConnectionState::kConnecting &&
      (mask & event_loop::EventFlag::kWritable) != 0) {
    // A nonblocking connect completes through the writable event; SO_ERROR
    // tells whether it succeeded.
    if (tcp::IsSocketError(fd)) {
      RS_LOG_DEBUG("socket error\n");
      conn->SetState(ConnectionState::kError);
      return event_loop::CallbackStatus::kError;
    }
    RS_LOG_DEBUG("connection state set to connected\n");
    conn->SetState(ConnectionState::kConnected);
    if (!conn->write_callback_) {
      loop->DeleteFileEvent(fd, event_loop::EventFlag::kWritable);
      conn->flags_ &= ~event_loop::EventFlag::kWritable;
    }
  }
  if (conn->write_callback_) {
    conn->write_callback_(conn);
  }
  return conn->State() == ConnectionState::kClosed ||
                 conn->State() == ConnectionState::kError
             ? event_loop::CallbackStatus::kError
             : event_loop::CallbackStatus::kOk;
}

int Connection::Wait(event_loop::EventFlag flag, int64_t timeout_ms) const {
  int r = event_loop::WaitForEvent(fd_, flag, timeout_ms);
  if (r < 0) {
    RS_LOG_DEBUG("conn sync %s failed for connection %d\n",
                 flag == event_loop::EventFlag::kReadable ? "read" : "write",
                 fd_);
    return -1;
  }
  if (r == 0) {
    RS_LOG_DEBUG("event wait timeout\n");
    return -1;
  }
  return r;
}
}  // namespace redis_simple::connection
