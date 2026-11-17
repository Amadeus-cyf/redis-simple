#ifndef REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_CLIENT_CONNECTION_H_
#define REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_CLIENT_CONNECTION_H_

#include <string_view>

#include "event_loop/loop.h"

namespace redis_simple {
class Server;
}  // namespace redis_simple

namespace redis_simple::client_connection {
// Mirrors Redis' protected mode: with no password configured, only clients
// connecting over loopback are accepted.
bool RejectedByProtectedMode(bool protected_mode, std::string_view password,
                             std::string_view client_ip);

// The listening socket's read callback: accepts pending connections on
// `fd`, a bounded number per call, and creates a client for each, on the
// server's next I/O thread when it has any.
event_loop::CallbackStatus AcceptConnectionCallback(event_loop::Loop* loop,
                                                    int fd, Server* server,
                                                    int mask);
}  // namespace redis_simple::client_connection

#endif  // REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_CLIENT_CONNECTION_H_
