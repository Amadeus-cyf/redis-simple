#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
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

int Run(int port) {
  const tcp::TcpAddrInfo remote("localhost", port);
  const int fd = tcp::TcpBindAndConnect(remote, {}, false);
  if (fd < 0) {
    RS_LOG_DEBUG("failed to connect to tcp integration server\n");
    return EXIT_FAILURE;
  }
  if (write(fd, kRequest.data(), kRequest.size()) !=
      static_cast<ssize_t>(kRequest.size())) {
    RS_LOG_DEBUG("failed to write tcp integration request\n");
    close(fd);
    return EXIT_FAILURE;
  }
  std::array<char, 16> buffer{};
  const ssize_t nread = ReadWithTimeout(fd, buffer.data(), buffer.size());
  close(fd);
  if (std::cmp_not_equal(nread, kResponse.size()) ||
      std::string_view(buffer.data(), nread) != kResponse) {
    RS_LOG_DEBUG("unexpected tcp response: %s\n", buffer.data());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
}  // namespace redis_simple

int main(int argc, char** argv) {
  try {
    return argc == 2 ? redis_simple::Run(std::stoi(argv[1])) : EXIT_FAILURE;
  } catch (...) {
    return EXIT_FAILURE;
  }
}
