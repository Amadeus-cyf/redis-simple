#ifndef REDIS_SIMPLE_TCP_TCP_H_
#define REDIS_SIMPLE_TCP_TCP_H_

#include <sys/socket.h>

#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::tcp {
enum class TcpStatusCode {
  kError = -1,
  kOk = 0,
};

constexpr int ToInt(TcpStatusCode status) { return static_cast<int>(status); }
// C++20 derives the reversed and != forms from this operator.
constexpr bool operator==(int value, TcpStatusCode status) {
  return value == ToInt(status);
}

struct TcpAddress {
  std::string ip;
  int port{-1};
  TcpAddress() = default;
  TcpAddress(std::string_view ip, int port) : ip(ip), port(port) {}
};

// The functions below return a descriptor or ToInt(TcpStatusCode::kOk) on
// success and ToInt(TcpStatusCode::kError), which is -1, on failure.

// Creates a close-on-exec stream socket with SO_REUSEADDR set.
int TcpCreateSocket(int domain, bool non_block);
// Connects to `remote` over IPv4, from `local` if given. With `non_block`
// the connection may still be in progress when the descriptor is returned;
// the socket turns writable when it completes, and IsSocketError tells
// whether it succeeded.
int TcpBindAndConnect(const TcpAddress& remote,
                      const std::optional<TcpAddress>& local,
                      bool non_block = true);
int TcpBind(int socket_fd, const TcpAddress& address);
int TcpListen(int socket_fd);
// Accepts a connection as a nonblocking, close-on-exec socket with Nagle's
// algorithm off and keepalive probes on, as Redis sets up its clients.
int TcpAccept(int socket_fd, TcpAddress* address);
// Make fd blocking or nonblocking.
int Block(int fd);
int NonBlock(int fd);
// Whether the socket has a pending error (SO_ERROR), or cannot be asked.
bool IsSocketError(int fd);
}  // namespace redis_simple::tcp

#endif  // REDIS_SIMPLE_TCP_TCP_H_
