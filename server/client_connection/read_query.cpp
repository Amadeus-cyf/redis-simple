#include "server/client_connection/read_query.h"

#include <sys/types.h>

#include "connection/connection.h"
#include "logging/logger.h"
#include "server/client.h"
#include "server/client_connection/write_reply.h"
#include "server/server.h"

namespace redis_simple::client_connection {
namespace {
void ReadQuery(Server* const server, Client* const client) {
  connection::Connection* const connection = client->Connection();
  const ssize_t nread = client->ReadQuery();
  if (nread <= 0) {
    if (connection->State() != connection::ConnectionState::kConnected) {
      server->RemoveClient(client);
    }
    return;
  }
  if (client->ProcessInputBuffer() == ClientStatus::kError) {
    RS_LOG_DEBUG("process query buffer failed\n");
  }
  if (server->Aof() != nullptr) {
    server->HoldReplies(client);
    return;
  }
  // Nothing has to reach a file first, so the replies leave while the
  // client's buffers are still in the cache.
  ReplyToQueries(server, client);
}
}  // namespace

void ReplyToQueries(Server* const server, Client* const client) {
  connection::Connection* const connection = client->Connection();
  if (connection->State() != connection::ConnectionState::kConnected) {
    server->RemoveClient(client);
    return;
  }
  if (client->ShouldPauseReads() && connection->UnsetReadCallback()) {
    client->SetReadsPaused(true);
  }
  WriteToClient(server, client);
}

connection::ConnectionCallback CreateReadQueryCallback(Server* const server,
                                                       Client* const client) {
  return [server, client](connection::Connection* /*connection*/) {
    ReadQuery(server, client);
  };
}
}  // namespace redis_simple::client_connection
