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
// Sends the replies to the queries `client` ran, once the server has flushed
// the AOF for them, and pauses reading while too much output is unsent.
void ReplyToQueries(Server* server, Client* client);
}  // namespace redis_simple::client_connection

#endif  // REDIS_SIMPLE_SERVER_CLIENT_CONNECTION_READ_QUERY_H_
