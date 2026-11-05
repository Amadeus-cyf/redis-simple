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
// The two halves of WriteToClient. WritePendingReplies only writes to the
// client's socket and touches no other state, so I/O threads may run it for
// different clients at once; it returns the bytes written, or -1 on error.
// FinishWrite then runs on the main thread with that result.
ssize_t WritePendingReplies(Client* client);
void FinishWrite(Server* server, Client* client, ssize_t written);
}  // namespace redis_simple::client_connection

#endif  // REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_WRITE_REPLY_H_
