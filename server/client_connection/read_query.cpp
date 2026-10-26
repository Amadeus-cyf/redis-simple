#include "server/client_connection/read_query.h"

#include <sys/types.h>

#include "connection/connection.h"
#include "logging/logger.h"
#include "server/aof.h"
#include "server/client.h"
#include "server/client_connection/write_reply.h"
#include "server/server.h"

namespace redis_simple::client_connection {
namespace {
void ReadQuery(Server* const server, Client* const client) {
  connection::Connection* const conn = client->Connection();
  const ssize_t nread = client->ReadQuery();
  if (nread <= 0) {
    if (conn->State() != connection::ConnectionState::kConnected) {
      server->RemoveClient(client);
    }
    return;
  }
  if (client->ProcessInputBuffer() == ClientStatus::kError) {
    RS_LOG_DEBUG("process query buffer failed\n");
  }
  // Persist this batch of mutations before any of its replies are sent.
  if (aof::Aof* const append_only_file = server->Aof();
      append_only_file != nullptr && !append_only_file->Flush()) {
    server->Stop();
    server->RemoveClient(client);
    return;
  }
  if (conn->State() != connection::ConnectionState::kConnected) {
    server->RemoveClient(client);
    return;
  }
  if (client->ShouldPauseReads() && conn->UnsetReadCallback()) {
    client->SetReadsPaused(true);
  }
  WriteToClient(server, client);
}
}  // namespace

connection::ConnectionCallback CreateReadQueryCallback(Server* const server,
                                                       Client* const client) {
  return [server, client](connection::Connection* /*conn*/) {
    ReadQuery(server, client);
  };
}
}  // namespace redis_simple::client_connection
