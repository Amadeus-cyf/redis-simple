#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "logging/logger.h"
#include "tcp/tcp.h"

namespace redis_simple {
namespace {
constexpr std::string_view kRequest = "ping";
constexpr std::string_view kResponse = "pong";

ssize_t ReadWithTimeout(int fd, char* buffer, size_t len) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  if (poll(&pfd, 1, 1000) <= 0) {
    return -1;
  }
  return read(fd, buffer, len);
}
}  // namespace

int Run(std::string_view port_file) {
  int s = tcp::TcpCreateSocket(AF_INET, false);
  if (s < 0) {
    std::cerr << "failed to create TCP integration socket\n";
    RS_LOG_DEBUG("failed to create socket\n");
    return EXIT_FAILURE;
  }
  constexpr int kPort = 0;
  const tcp::TcpAddrInfo local("localhost", kPort);
  if (tcp::TcpBind(s, local) != tcp::TcpStatusCode::kOk) {
    std::cerr << "failed to bind TCP integration socket: "
              << std::strerror(errno) << '\n';
    RS_LOG_DEBUG("failed to bind tcp integration server\n");
    close(s);
    return EXIT_FAILURE;
  }
  if (tcp::TcpListen(s) != tcp::TcpStatusCode::kOk) {
    std::cerr << "failed to listen on TCP integration socket\n";
    RS_LOG_DEBUG("failed to listen on tcp integration server\n");
    close(s);
    return EXIT_FAILURE;
  }
  sockaddr_in bound_address{};
  socklen_t bound_address_size = sizeof(bound_address);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&bound_address),
                  &bound_address_size) < 0) {
    std::cerr << "failed to read TCP integration port: " << std::strerror(errno)
              << '\n';
    close(s);
    return EXIT_FAILURE;
  }
  std::ofstream ready_file{std::string(port_file)};
  ready_file << ntohs(bound_address.sin_port);
  ready_file.close();
  if (!ready_file) {
    std::cerr << "failed to publish TCP integration port\n";
    close(s);
    return EXIT_FAILURE;
  }

  tcp::TcpAddrInfo remote_addr;
  const int remote_fd = tcp::TcpAccept(s, &remote_addr);
  if (remote_fd < 0) {
    std::cerr << "failed to accept TCP integration client\n";
    RS_LOG_DEBUG("failed to accept tcp integration client\n");
    close(s);
    return EXIT_FAILURE;
  }
  RS_LOG_DEBUG("accept connection from %s:%d\n", remote_addr.ip.c_str(),
               remote_addr.port);

  std::array<char, 16> buffer{};
  const ssize_t nread =
      ReadWithTimeout(remote_fd, buffer.data(), buffer.size());
  if (std::cmp_not_equal(nread, kRequest.size()) ||
      std::string_view(buffer.data(), nread) != kRequest) {
    RS_LOG_DEBUG("unexpected tcp request: %s\n", buffer.data());
    close(remote_fd);
    close(s);
    return EXIT_FAILURE;
  }
  if (write(remote_fd, kResponse.data(), kResponse.size()) !=
      static_cast<ssize_t>(kResponse.size())) {
    RS_LOG_DEBUG("failed to write tcp integration response\n");
    close(remote_fd);
    close(s);
    return EXIT_FAILURE;
  }
  close(remote_fd);
  close(s);
  return EXIT_SUCCESS;
}
}  // namespace redis_simple

int main(int argc, char** argv) {
  try {
    return argc == 2 ? redis_simple::Run(argv[1]) : EXIT_FAILURE;
  } catch (...) {
    return EXIT_FAILURE;
  }
}
