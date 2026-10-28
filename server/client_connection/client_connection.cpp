#include "server/client_connection/client_connection.h"

#include <memory>
#include <string_view>
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

constexpr std::string_view kProtectedModeError =
    "-DENIED Running in protected mode because protected mode is enabled and "
    "no password is set. Only loopback connections are accepted. Set a "
    "password with --requirepass, bind to a private interface, or disable "
    "protected mode with --protected-mode no.\r\n";

// Returns false once the listener has no more pending connections.
bool AcceptOne(int listener_fd, Server* const server) {
  auto conn = std::make_unique<connection::Connection>(
      connection::Context{.loop = server->Loop(), .fd = -1});
  conn->SetState(connection::ConnectionState::kAccepting);
  connection::AddressInfo addr_info;
  if (conn->Accept(listener_fd, &addr_info) ==
      connection::ConnectionStatus::kError) {
    return false;
  }
  RS_LOG_DEBUG("accept connection from %s:%d with fd = %d\n",
               addr_info.ip.c_str(), addr_info.port, conn->Descriptor());
  if (RejectedByProtectedMode(server->ProtectedMode(), server->RequirePass(),
                              addr_info.ip)) {
    RS_LOG_WARN("rejected %s:%d: protected mode\n", addr_info.ip.c_str(),
                addr_info.port);
    // Best effort: the socket is new and empty, so one write normally fits.
    // Dropping `conn` closes it.
    conn->Write(kProtectedModeError);
    return true;
  }
  auto client = Client::Create(std::move(conn), server->Db(), server->Aof());
  client->RequirePassword(server->RequirePass());
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

bool RejectedByProtectedMode(bool protected_mode, std::string_view password,
                             std::string_view client_ip) {
  if (!protected_mode || !password.empty()) {
    return false;
  }
  // 127.0.0.0/8 is loopback. Connections are IPv4-only today.
  return !client_ip.starts_with("127.");
}

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
