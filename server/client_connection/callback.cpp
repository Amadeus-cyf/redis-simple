#include "server/client_connection/callback.h"

#include "server/client_connection/read_query.h"
#include "server/client_connection/write_reply.h"

namespace redis_simple::client_connection {
connection::ConnectionCallback CreateCallback(CallbackType type,
                                              Server* const server,
                                              Client* const client) {
  switch (type) {
    case CallbackType::kReadQuery:
      return CreateReadQueryCallback(server, client);
    case CallbackType::kWriteReply:
      return CreateWriteReplyCallback(server, client);
  }
  return nullptr;
}
}  // namespace redis_simple::client_connection
