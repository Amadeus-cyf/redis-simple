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
  // With I/O threads, the read happens with the next batch.
  if (server->ThreadedIo()) {
    server->QueueRead(client);
    return;
  }
  if (!RunQueries(server, client, client->ReadQuery())) {
    return;
  }
  // Persist this batch of mutations before any of its replies are sent.
  if (aof::Aof* const append_only_file = server->Aof();
      append_only_file != nullptr && !append_only_file->Flush()) {
    server->Stop();
    server->RemoveClient(client);
    return;
  }
  FinishQueries(server, client);
}
}  // namespace

bool RunQueries(Server* const server, Client* const client, ssize_t nread) {
  if (nread <= 0) {
    if (client->Connection()->State() !=
        connection::ConnectionState::kConnected) {
      server->RemoveClient(client);
    }
    return false;
  }
  if (client->ProcessInputBuffer() == ClientStatus::kError) {
    RS_LOG_DEBUG("process query buffer failed\n");
  }
  return true;
}

void FinishQueries(Server* const server, Client* const client) {
  connection::Connection* const conn = client->Connection();
  if (conn->State() != connection::ConnectionState::kConnected) {
    server->RemoveClient(client);
    return;
  }
  if (client->ShouldPauseReads() && conn->UnsetReadCallback()) {
    client->SetReadsPaused(true);
  }
  if (server->ThreadedIo()) {
    server->QueueWrite(client);
  } else {
    WriteToClient(server, client);
  }
}

connection::ConnectionCallback CreateReadQueryCallback(Server* const server,
                                                       Client* const client) {
  return [server, client](connection::Connection* /*conn*/) {
    ReadQuery(server, client);
  };
}
}  // namespace redis_simple::client_connection
