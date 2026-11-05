#ifndef REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_READ_QUERY_H_
#define REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_READ_QUERY_H_

#include <sys/types.h>

#include "connection/connection_callback.h"

namespace redis_simple {
class Client;
class Server;
}  // namespace redis_simple

namespace redis_simple::client_connection {
connection::ConnectionCallback CreateReadQueryCallback(Server* server,
                                                       Client* client);
// Main-thread steps after a client's read returned `nread`. RunQueries runs
// the commands it completed and returns whether the client is still open.
// After the server flushes the AOF, FinishQueries pauses reads if output is
// backed up and writes the replies, or queues them for the next I/O batch.
bool RunQueries(Server* server, Client* client, ssize_t nread);
void FinishQueries(Server* server, Client* client);
}  // namespace redis_simple::client_connection

#endif  // REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_READ_QUERY_H_
