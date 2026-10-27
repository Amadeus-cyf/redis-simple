#include <cstdint>
#include <string>
#include <string_view>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple::command::session {
namespace {
std::string EncodeHello(reply::ProtocolVersion protocol) {
  constexpr size_t kFieldCount = 7;
  std::string encoded = reply::FromMapHeader(kFieldCount, protocol);
  reply::AppendBulkString("server", &encoded);
  reply::AppendBulkString("redis_simple", &encoded);
  reply::AppendBulkString("version", &encoded);
  reply::AppendBulkString("0.1.0", &encoded);
  reply::AppendBulkString("proto", &encoded);
  encoded.append(reply::FromInt64(static_cast<int64_t>(protocol)));
  reply::AppendBulkString("id", &encoded);
  encoded.append(reply::FromInt64(0));
  reply::AppendBulkString("mode", &encoded);
  reply::AppendBulkString("standalone", &encoded);
  reply::AppendBulkString("role", &encoded);
  reply::AppendBulkString("master", &encoded);
  reply::AppendBulkString("modules", &encoded);
  encoded.append(reply::FromArrayHeader(0));
  return encoded;
}
}  // namespace

void HandleAuth(Client* const client) {
  const auto& args = client->Args();
  const std::string_view password = client->RequiredPassword();
  if (password.empty()) {
    client->AddReply(reply::FromError(
        "ERR AUTH <password> called without any password configured for the "
        "default user. Are you sure your configuration is correct?"));
    return;
  }
  // AUTH <password> or AUTH <username> <password>; only "default" exists.
  const bool known_user = args.size() == 1 || args[0] == "default";
  if (!known_user || !utils::ConstantTimeEquals(args.back(), password)) {
    client->AddReply(reply::FromError(
        "WRONGPASS invalid username-password pair or user is disabled."));
    return;
  }
  client->SetAuthenticated(true);
  client->AddReply(reply::kOk);
}

void HandleHello(Client* const client) {
  const auto& args = client->Args();
  auto protocol = client->Protocol();
  if (!args.empty()) {
    int64_t version = 0;
    if (!utils::ToInt64(args[0], &version) || (version != 2 && version != 3)) {
      client->AddReply(
          reply::FromError("NOPROTO unsupported protocol version"));
      return;
    }
    protocol = version == 2 ? reply::ProtocolVersion::kResp2
                            : reply::ProtocolVersion::kResp3;
    client->SetProtocol(protocol);
  }
  client->AddReply(EncodeHello(protocol));
}

void HandlePing(Client* const client) {
  constexpr std::string_view kPong = "+PONG\r\n";
  const auto& args = client->Args();
  if (args.empty()) {
    client->AddReply(kPong);
  } else {
    client->AddBulkReply(args[0]);
  }
}

void HandleEcho(Client* const client) {
  client->AddBulkReply(client->Args()[0]);
}

void HandleQuit(Client* const client) {
  client->AddReply(reply::kOk);
  client->CloseAfterReply();
}
}  // namespace redis_simple::command::session
