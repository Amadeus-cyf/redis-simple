#pragma once

#include "connection/connection_callback.h"

namespace redis_simple {
class Client;
class Server;
}  // namespace redis_simple

namespace redis_simple::client_connection {
connection::ConnectionCallback CreateWriteReplyCallback(Server* server,
                                                        Client* client);

// Writes pending replies without waiting for a writable event. A write
// callback is installed only while the socket cannot take all pending output.
void WriteToClient(Server* server, Client* client);
}  // namespace redis_simple::client_connection
