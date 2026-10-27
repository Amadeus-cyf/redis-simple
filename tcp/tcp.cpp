#include "tcp/tcp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>

#include "logging/logger.h"

namespace redis_simple::tcp {
namespace {
// Keep the listen queue small for this single-threaded test server.
constexpr int kBacklog = 3;

int SetCloseOnExec(int fd);

int TcpSetReuseAddr(int socket_fd) {
  int yes = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ==
      -1) {
    return ToInt(TcpStatusCode::kError);
  }
  return ToInt(TcpStatusCode::kOk);
}

int TcpGenericCreateSocket(int domain, int type, int protocol, bool non_block) {
  // Centralize socket setup so callers do not forget reuseaddr or nonblocking
  // mode when creating listening and client sockets.
  int socket_fd = socket(domain, type, protocol);
  if (socket_fd < 0) {
    return ToInt(TcpStatusCode::kError);
  }
  if (SetCloseOnExec(socket_fd) == TcpStatusCode::kError ||
      TcpSetReuseAddr(socket_fd) < 0) {
    close(socket_fd);
    return ToInt(TcpStatusCode::kError);
  }
  if (non_block && NonBlock(socket_fd) == TcpStatusCode::kError) {
    close(socket_fd);
    return ToInt(TcpStatusCode::kError);
  }
  return socket_fd;
}

int TcpGenericAccept(int socket_fd, sockaddr* addr, socklen_t* len) {
  int s = accept(socket_fd, addr, len);
  if (s == -1) {
    return ToInt(TcpStatusCode::kError);
  }
  return s;
}

int SetBlock(int fd, bool block) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    return ToInt(TcpStatusCode::kError);
  }
  bool is_non_block = (flags & O_NONBLOCK) != 0;
  if (is_non_block == !block) {
    return ToInt(TcpStatusCode::kOk);
  }
  block ? flags &= ~O_NONBLOCK : flags |= O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags) < 0 ? ToInt(TcpStatusCode::kError)
                                       : ToInt(TcpStatusCode::kOk);
}

int SetCloseOnExec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) {
    return ToInt(TcpStatusCode::kError);
  }
  if ((flags & FD_CLOEXEC) != 0) {
    return ToInt(TcpStatusCode::kOk);
  }
  flags |= FD_CLOEXEC;
  return fcntl(fd, F_SETFD, flags) < 0 ? ToInt(TcpStatusCode::kError)
                                       : ToInt(TcpStatusCode::kOk);
}
}  // namespace

int TcpCreateSocket(int domain, bool non_block) {
  return TcpGenericCreateSocket(domain, SOCK_STREAM, 0, non_block);
}

int TcpBindAndConnect(const TcpAddrInfo& remote,
                      const std::optional<TcpAddrInfo>& local, bool non_block) {
  addrinfo hints{};
  addrinfo* info = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(remote.ip.c_str(), std::to_string(remote.port).c_str(),
                  &hints, &info) != 0) {
    return ToInt(TcpStatusCode::kError);
  }
  int socket_fd = -1;
  for (const addrinfo* p = info; p != nullptr; p = p->ai_next) {
    socket_fd = TcpGenericCreateSocket(p->ai_family, p->ai_socktype,
                                       p->ai_protocol, non_block);
    if (socket_fd < 0) {
      continue;
    }

    if (local.has_value() && TcpBind(socket_fd, *local) < 0) {
      RS_LOG_DEBUG("bind error: %s\n", std::strerror(errno));
      close(socket_fd);
      socket_fd = -1;
      continue;
    }

    if (connect(socket_fd, p->ai_addr, p->ai_addrlen) < 0) {
      // For nonblocking sockets, EINPROGRESS means the connection attempt is
      // still valid and completion will be reported by the writable event.
      if (!non_block || errno != EINPROGRESS) {
        RS_LOG_DEBUG("connect error: %s\n", std::strerror(errno));
        close(socket_fd);
        socket_fd = -1;
        continue;
      }
      RS_LOG_DEBUG("connect in progress %d\n", socket_fd);
    }
    break;
  }
  freeaddrinfo(info);
  return socket_fd != -1 ? socket_fd : ToInt(TcpStatusCode::kError);
}

int TcpAccept(int socket_fd, TcpAddrInfo* const addr_info) {
  sockaddr_storage sa{};
  socklen_t len = sizeof(sa);
  int remote_fd = -1;
  // accept() may be interrupted by a signal; retry those transient failures.
  while (true) {
    remote_fd =
        TcpGenericAccept(socket_fd, reinterpret_cast<sockaddr*>(&sa), &len);
    if (remote_fd != -1 || errno != EINTR) {
      break;
    }
  }
  if (remote_fd < 0) {
    return ToInt(TcpStatusCode::kError);
  }
  if (NonBlock(remote_fd) == TcpStatusCode::kError ||
      SetCloseOnExec(remote_fd) == TcpStatusCode::kError) {
    close(remote_fd);
    return ToInt(TcpStatusCode::kError);
  }
  if (addr_info != nullptr) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&sa);
    std::array<char, INET_ADDRSTRLEN> ip{};
    if (inet_ntop(AF_INET, &address->sin_addr, ip.data(), ip.size()) ==
        nullptr) {
      close(remote_fd);
      return ToInt(TcpStatusCode::kError);
    }
    addr_info->ip = ip.data();
    addr_info->port = ntohs(address->sin_port);
  }
  return remote_fd;
}

int TcpBind(int socket_fd, const TcpAddrInfo& addr_info) {
  addrinfo hints{};
  addrinfo* info = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  const int address_status =
      getaddrinfo(addr_info.ip.c_str(), std::to_string(addr_info.port).c_str(),
                  &hints, &info);
  if (address_status != 0) {
    RS_LOG_DEBUG("getaddrinfo failed: %s\n", gai_strerror(address_status));
    return ToInt(TcpStatusCode::kError);
  }
  int r = ToInt(TcpStatusCode::kError);
  for (const addrinfo* p = info; p != nullptr; p = p->ai_next) {
    if (bind(socket_fd, p->ai_addr, p->ai_addrlen) < 0) {
      continue;
    }
    RS_LOG_DEBUG("bind success\n");
    r = ToInt(TcpStatusCode::kOk);
    break;
  }
  freeaddrinfo(info);
  return r;
}

int TcpListen(int socket_fd) {
  if (listen(socket_fd, kBacklog) < 0) {
    return ToInt(TcpStatusCode::kError);
  }
  return ToInt(TcpStatusCode::kOk);
}

int NonBlock(int socket_fd) { return SetBlock(socket_fd, false); }

int Block(int socket_fd) { return SetBlock(socket_fd, true); }

bool IsSocketError(int fd) {
  int socket_error = 0;
  socklen_t error_length = sizeof(socket_error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0) {
    return true;
  }
  RS_LOG_DEBUG("sock err %d\n", socket_error);
  return socket_error != 0;
}
}  // namespace redis_simple::tcp
