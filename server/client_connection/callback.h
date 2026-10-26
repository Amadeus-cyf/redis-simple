#pragma once

#include "connection/connection_callback.h"

namespace redis_simple {
class Client;
class Server;
}  // namespace redis_simple

namespace redis_simple::client_connection {
enum class CallbackType {
  kReadQuery,
  kWriteReply,
};

// `server` and `client` must outlive the connection the callback is set on.
connection::ConnectionCallback CreateCallback(CallbackType type, Server* server,
                                              Client* client);
}  // namespace redis_simple::client_connection
