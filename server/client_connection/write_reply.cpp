#include "server/client_connection/write_reply.h"

#include <sys/types.h>

#include <cstddef>

#include "connection/connection.h"
#include "logging/logger.h"
#include "server/client.h"
#include "server/client_connection/callback.h"
#include "server/server.h"

namespace redis_simple::client_connection {
namespace {
// Bounds the bytes written to one client per event so a large reply cannot
// starve other clients, like Redis' NET_MAX_WRITES_PER_EVENT.
constexpr size_t kMaxBytesPerEvent = size_t{64} * 1024;
}  // namespace

void WriteToClient(Server* const server, Client* const client) {
  FinishWrite(server, client, WritePendingReplies(client));
}

ssize_t WritePendingReplies(Client* const client) {
  size_t total_written = 0;
  while (client->HasPendingReplies() && total_written < kMaxBytesPerEvent) {
    const ssize_t written = client->SendReply();
    if (written < 0) {
      return -1;
    }
    if (written == 0) {
      break;
    }
    total_written += static_cast<size_t>(written);
  }
  return static_cast<ssize_t>(total_written);
}

void FinishWrite(Server* const server, Client* const client, ssize_t written) {
  connection::Connection* const conn = client->Connection();
  if (written < 0 && conn->State() != connection::ConnectionState::kConnected) {
    server->RemoveClient(client);
    return;
  }
  if (client->HasPendingReplies()) {
    if (!conn->HasWriteCallback() &&
        !conn->SetWriteCallback(
            CreateCallback(CallbackType::kWriteReply, server, client))) {
      RS_LOG_DEBUG("failed to install the write callback\n");
    }
  } else {
    if (conn->HasWriteCallback() && !conn->UnsetWriteCallback()) {
      RS_LOG_DEBUG("failed to unset the write callback\n");
    }
    if (client->ShouldCloseAfterReply()) {
      server->RemoveClient(client);
      return;
    }
  }
  if (client->ShouldResumeReads() &&
      conn->SetReadCallback(
          CreateCallback(CallbackType::kReadQuery, server, client))) {
    client->SetReadsPaused(false);
  }
}

connection::ConnectionCallback CreateWriteReplyCallback(Server* const server,
                                                        Client* const client) {
  return [server, client](connection::Connection* /*conn*/) {
    WriteToClient(server, client);
  };
}
}  // namespace redis_simple::client_connection
