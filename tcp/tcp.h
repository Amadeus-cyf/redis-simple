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

int TcpCreateSocket(int domain, bool non_block);
int TcpBindAndConnect(const TcpAddress& remote,
                      const std::optional<TcpAddress>& local,
                      bool non_block = true);
int TcpBind(int socket_fd, const TcpAddress& address);
int TcpListen(int socket_fd);
int TcpAccept(int socket_fd, TcpAddress* address);
int Block(int fd);
int NonBlock(int fd);
bool IsSocketError(int fd);
}  // namespace redis_simple::tcp

#endif  // REDIS_SIMPLE_TCP_TCP_H_
