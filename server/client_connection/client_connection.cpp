#include "client_connection.h"

#include <memory>
#include <utility>

#include "logging/logger.h"
#include "server/client.h"
#include "server/client_connection/callback.h"
#include "server/server.h"

namespace redis_simple::client_connection {
namespace {
// Accept a bounded burst per readable event, like Redis'
// MAX_ACCEPTS_PER_CALL, so a connection storm cannot starve other clients.
constexpr int kMaxAcceptsPerCall = 1000;

// Returns false once the listener has no more pending connections.
bool AcceptOne(int listener_fd, Server* const server) {
  connection::Context ctx;
  ctx.loop = server->Loop();
  ctx.fd = -1;
  auto conn = std::make_unique<connection::Connection>(ctx);
  conn->SetState(connection::ConnectionState::kAccepting);
  connection::AddressInfo addr_info;
  if (conn->Accept(listener_fd, &addr_info) ==
      connection::ConnectionStatus::kError) {
    return false;
  }
  RS_LOG_DEBUG("accept connection from %s:%d with fd = %d\n",
               addr_info.ip.c_str(), addr_info.port, conn->Descriptor());
  auto client = Client::Create(std::move(conn), server->Db(), server->Aof());
  Client* const client_ptr = client.get();
  if (!client_ptr->Connection()->SetReadCallback(
          CreateCallback(CallbackType::kReadQuery, server, client_ptr))) {
    RS_LOG_DEBUG("failed to set the read callback\n");
    // Dropping `client` closes this connection; keep accepting the rest.
    return true;
  }
  server->AddClient(std::move(client));
  return true;
}
}  // namespace

event_loop::CallbackStatus AcceptConnectionCallback(event_loop::Loop* /*loop*/,
                                                    int fd, Server* server,
                                                    int /*mask*/) {
  if (server == nullptr) {
    RS_LOG_DEBUG("invalid server / event loop\n");
    return event_loop::CallbackStatus::kError;
  }
  for (int i = 0; i < kMaxAcceptsPerCall; ++i) {
    if (!AcceptOne(fd, server)) {
      break;
    }
  }
  return event_loop::CallbackStatus::kOk;
}
}  // namespace redis_simple::client_connection
