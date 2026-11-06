#ifndef REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_WRITE_REPLY_H_
#define REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_WRITE_REPLY_H_

#include <sys/types.h>

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
// Writes pending replies, at most one event's worth, and returns the bytes
// written or -1 on error. It touches only the client's socket and output, so
// a client's I/O thread uses it too.
ssize_t WritePendingReplies(Client* client);
}  // namespace redis_simple::client_connection

#endif  // REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_WRITE_REPLY_H_
