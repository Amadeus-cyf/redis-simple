#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/reply.h"
#include "server/version.h"
#include "utils/strings.h"

namespace redis_simple::commands::session {
namespace {
constexpr std::string_view kWrongPass =
    "WRONGPASS invalid username-password pair or user is disabled.";

std::string EncodeHello(const Client& client) {
  constexpr size_t kFieldCount = 7;
  const reply::ProtocolVersion protocol = client.Protocol();
  std::string encoded = reply::FromMapHeader(kFieldCount, protocol);
  reply::AppendBulkString("server", &encoded);
  reply::AppendBulkString("redis_simple", &encoded);
  reply::AppendBulkString("version", &encoded);
  reply::AppendBulkString(kServerVersion, &encoded);
  reply::AppendBulkString("proto", &encoded);
  encoded.append(reply::FromInt64(static_cast<int64_t>(protocol)));
  reply::AppendBulkString("id", &encoded);
  encoded.append(reply::FromInt64(static_cast<int64_t>(client.Id())));
  reply::AppendBulkString("mode", &encoded);
  reply::AppendBulkString("standalone", &encoded);
  reply::AppendBulkString("role", &encoded);
  reply::AppendBulkString("master", &encoded);
  reply::AppendBulkString("modules", &encoded);
  encoded.append(reply::FromArrayHeader(0));
  return encoded;
}

// Redis' ACLAuthenticateUser for the only user, "default": without a
// password it is nopass and accepts any password.
bool Authenticate(const Client& client, std::string_view user,
                  std::string_view password) {
  if (user != "default") {
    return false;
  }
  const std::string_view required = client.Server().required_password;
  return required.empty() || utils::ConstantTimeEquals(password, required);
}

// Redis' validateClientAttr: printable ASCII without spaces.
bool ValidClientAttribute(std::string_view value) {
  return std::ranges::all_of(value,
                             [](char c) { return c >= '!' && c <= '~'; });
}
}  // namespace

void HandleAuth(Client* const client) {
  // Redis' authCommand: AUTH <password> or AUTH <username> <password>.
  const commands::CommandArgs& args = client->Args();
  if (args.size() > 2) {
    client->AddReply(reply::kSyntaxError);
    return;
  }
  if (args.size() == 1 && client->Server().required_password.empty()) {
    client->AddReply(reply::FromError(
        "ERR AUTH <password> called without any password configured for the "
        "default user. Are you sure your configuration is correct?"));
    return;
  }
  const std::string_view user = args.size() == 1 ? "default" : args[0];
  if (!Authenticate(*client, user, args.back())) {
    client->AddReply(reply::FromError(kWrongPass));
    return;
  }
  client->SetAuthenticated(true);
  client->AddReply(reply::kOk);
}

void HandleHello(Client* const client) {
  // Redis' helloCommand: [protover [AUTH username password] [SETNAME name]].
  const commands::CommandArgs& args = client->Args();
  int64_t version = 0;
  size_t next = 0;
  if (!args.empty()) {
    if (!utils::ToInt64(args[0], &version)) {
      client->AddReply(reply::FromError(
          "ERR Protocol version is not an integer or out of range"));
      return;
    }
    if (version < 2 || version > 3) {
      client->AddReply(
          reply::FromError("NOPROTO unsupported protocol version"));
      return;
    }
    next = 1;
  }
  std::optional<std::pair<std::string_view, std::string_view>> credentials;
  std::optional<std::string_view> name;
  for (size_t i = next; i < args.size(); ++i) {
    const size_t remaining = args.size() - 1 - i;
    if (utils::EqualsIgnoreCase(args[i], "AUTH") && remaining >= 2) {
      credentials.emplace(args[i + 1], args[i + 2]);
      i += 2;
    } else if (utils::EqualsIgnoreCase(args[i], "SETNAME") && remaining >= 1) {
      if (!ValidClientAttribute(args[i + 1])) {
        client->AddReply(reply::FromError(
            "ERR Client names cannot contain spaces, newlines or special "
            "characters."));
        return;
      }
      name = args[i + 1];
      ++i;
    } else {
      client->AddReply(reply::FromError("ERR Syntax error in HELLO option '" +
                                        std::string(args[i]) + "'"));
      return;
    }
  }
  if (credentials.has_value()) {
    if (!Authenticate(*client, credentials->first, credentials->second)) {
      client->AddReply(reply::FromError(kWrongPass));
      return;
    }
    client->SetAuthenticated(true);
  }
  if (!client->Authenticated()) {
    client->AddReply(reply::FromError(
        "NOAUTH HELLO must be called with the client already authenticated, "
        "otherwise the HELLO <proto> AUTH <user> <pass> option can be used to "
        "authenticate the client and select the RESP protocol version at the "
        "same time"));
    return;
  }
  if (name.has_value()) {
    client->SetName(*name);
  }
  if (version != 0) {
    client->SetProtocol(version == 2 ? reply::ProtocolVersion::kResp2
                                     : reply::ProtocolVersion::kResp3);
  }
  client->AddReply(EncodeHello(*client));
}

void HandleClient(Client* const client) {
  // The CLIENT subcommands a connection uses for itself.
  const commands::CommandArgs& args = client->Args();
  const std::string_view subcommand = args[0];
  const auto check_arity = [&](size_t count, std::string_view name) {
    if (args.size() == count) {
      return true;
    }
    client->AddReply(
        reply::FromError("ERR wrong number of arguments for 'client|" +
                         std::string(name) + "' command"));
    return false;
  };
  if (utils::EqualsIgnoreCase(subcommand, "ID")) {
    if (check_arity(1, "id")) {
      client->AddReply(reply::FromInt64(static_cast<int64_t>(client->Id())));
    }
  } else if (utils::EqualsIgnoreCase(subcommand, "GETNAME")) {
    if (check_arity(1, "getname")) {
      if (client->Name().empty()) {
        client->AddReply(reply::NullView(client->Protocol()));
      } else {
        client->AddBulkReply(client->Name());
      }
    }
  } else if (utils::EqualsIgnoreCase(subcommand, "SETNAME")) {
    if (!check_arity(2, "setname")) {
      return;
    }
    if (!ValidClientAttribute(args[1])) {
      client->AddReply(reply::FromError(
          "ERR Client names cannot contain spaces, newlines or special "
          "characters."));
      return;
    }
    client->SetName(args[1]);
    client->AddReply(reply::kOk);
  } else if (utils::EqualsIgnoreCase(subcommand, "SETINFO")) {
    // Client libraries announce themselves; nothing reads it back here.
    if (!check_arity(3, "setinfo")) {
      return;
    }
    if (!utils::EqualsIgnoreCase(args[1], "lib-name") &&
        !utils::EqualsIgnoreCase(args[1], "lib-ver")) {
      client->AddReply(reply::FromError("ERR Unrecognized option '" +
                                        std::string(args[1]) + "'"));
      return;
    }
    if (!ValidClientAttribute(args[2])) {
      client->AddReply(reply::FromError(
          "ERR " + std::string(args[1]) +
          " cannot contain spaces, newlines or special characters."));
      return;
    }
    client->AddReply(reply::kOk);
  } else {
    client->AddReply(reply::FromError("ERR unknown subcommand '" +
                                      std::string(subcommand.substr(0, 128)) +
                                      "'. Try CLIENT HELP."));
  }
}

void HandleSelect(Client* const client) {
  // One database, as in Redis' cluster mode, but out-of-range indexes get
  // Redis' standalone error.
  int64_t index = 0;
  if (!utils::ToInt64(client->Args()[0], &index)) {
    client->AddReply(reply::kNotAnIntegerError);
    return;
  }
  // Redis reads the index with getIntFromObjectOrReply, which has its own
  // error for an integer that does not fit an int.
  if (index < std::numeric_limits<int32_t>::min() ||
      index > std::numeric_limits<int32_t>::max()) {
    client->AddReply(
        reply::FromError("ERR value is out of range, value must between "
                         "-2147483648 and 2147483647"));
    return;
  }
  if (index != 0) {
    client->AddReply(reply::FromError("ERR DB index is out of range"));
    return;
  }
  client->AddReply(reply::kOk);
}

void HandlePing(Client* const client) {
  constexpr std::string_view kPong = "+PONG\r\n";
  const commands::CommandArgs& args = client->Args();
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
}  // namespace redis_simple::commands::session
