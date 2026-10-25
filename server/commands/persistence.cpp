#include <cstddef>
#include <string>
#include <string_view>

#include "server/aof.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple::command::persistence {
namespace {
void AppendField(std::string_view name, std::string_view value,
                 std::string* const output) {
  output->append(name).push_back(':');
  output->append(value).append("\r\n");
}

void AppendField(std::string_view name, size_t value,
                 std::string* const output) {
  AppendField(name, std::to_string(value), output);
}
}  // namespace

void HandleBgRewriteAof(Client* const client) {
  auto* const aof = client->Aof();
  if (aof == nullptr) {
    client->AddReply(reply::FromError("ERR append only file is disabled"));
    return;
  }

  switch (aof->StartRewrite(client->Db())) {
    case aof::RewriteResult::kStarted:
      client->AddReply(
          reply::FromString("Background append only file rewriting started"));
      return;
    case aof::RewriteResult::kInProgress:
      client->AddReply(reply::FromError(
          "ERR background append only file rewrite already in progress"));
      return;
    case aof::RewriteResult::kError:
      client->AddReply(
          reply::FromError("ERR background append only file rewrite failed"));
      return;
  }
}

void HandleInfo(Client* const client) {
  const auto& args = client->Args();
  if (!args.empty() && !utils::EqualsIgnoreCase(args[0], "persistence") &&
      !utils::EqualsIgnoreCase(args[0], "all") &&
      !utils::EqualsIgnoreCase(args[0], "default")) {
    client->AddBulkReply("");
    return;
  }

  std::string info = "# Persistence\r\n";
  const auto* const append_only_file = client->Aof();
  AppendField("aof_enabled", append_only_file == nullptr ? "0" : "1", &info);
  if (append_only_file == nullptr) {
    AppendField("aof_rewrite_in_progress", "0", &info);
    AppendField("aof_last_bgrewrite_status", "none", &info);
    AppendField("aof_last_error", "none", &info);
    AppendField("aof_current_size", size_t{0}, &info);
    AppendField("aof_base_size", size_t{0}, &info);
    AppendField("aof_pending_bytes", size_t{0}, &info);
    client->AddBulkReply(info);
    return;
  }

  const auto state = append_only_file->State();
  AppendField("aof_rewrite_in_progress", state.rewrite_in_progress ? "1" : "0",
              &info);
  AppendField("aof_last_bgrewrite_status",
              aof::RewriteStatusName(state.rewrite_status), &info);
  AppendField("aof_last_error", aof::ErrorName(state.last_error), &info);
  AppendField("aof_current_size", state.current_size, &info);
  AppendField("aof_base_size", state.base_size, &info);
  AppendField("aof_pending_bytes", state.pending_bytes, &info);
  client->AddBulkReply(info);
}
}  // namespace redis_simple::command::persistence
