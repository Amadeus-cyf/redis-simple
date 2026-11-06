#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "server/aof.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/io_thread.h"
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

namespace {
void AppendPersistenceSection(const aof::Aof* const append_only_file,
                              std::string* const info) {
  info->append("# Persistence\r\n");
  AppendField("aof_enabled", append_only_file == nullptr ? "0" : "1", info);
  if (append_only_file == nullptr) {
    AppendField("aof_rewrite_in_progress", "0", info);
    AppendField("aof_last_bgrewrite_status", "none", info);
    AppendField("aof_last_write_status", "ok", info);
    AppendField("aof_rewrites_consecutive_failures", "0", info);
    AppendField("aof_last_error", "none", info);
    AppendField("aof_current_size", size_t{0}, info);
    AppendField("aof_base_size", size_t{0}, info);
    AppendField("aof_pending_bytes", size_t{0}, info);
    return;
  }
  const aof::AofState state = append_only_file->State();
  AppendField("aof_rewrite_in_progress", state.rewrite_in_progress ? "1" : "0",
              info);
  AppendField("aof_last_bgrewrite_status",
              aof::RewriteStatusName(state.rewrite_status), info);
  AppendField("aof_last_write_status", state.accepting_writes ? "ok" : "err",
              info);
  AppendField("aof_rewrites_consecutive_failures",
              state.consecutive_rewrite_failures, info);
  AppendField("aof_last_error", aof::ErrorName(state.last_error), info);
  AppendField("aof_current_size", state.current_size, info);
  AppendField("aof_base_size", state.base_size, info);
  AppendField("aof_pending_bytes", state.pending_bytes, info);
}

using IoThreadList = std::vector<std::unique_ptr<IoThread>>;

// Threaded I/O fields of Redis' stats section: totals over the I/O threads.
void AppendStatsSection(const IoThreadList* const threads,
                        std::string* const info) {
  uint64_t reads = 0;
  uint64_t writes = 0;
  const bool active = threads != nullptr && !threads->empty();
  if (active) {
    for (const std::unique_ptr<IoThread>& thread : *threads) {
      const IoThreadInfo thread_info = thread->Info();
      reads += thread_info.reads_processed;
      writes += thread_info.writes_processed;
    }
  }
  info->append("# Stats\r\n");
  AppendField("io_threads_active", active ? "1" : "0", info);
  AppendField("io_threaded_reads_processed", static_cast<size_t>(reads), info);
  AppendField("io_threaded_writes_processed", static_cast<size_t>(writes),
              info);
}

// Redis 8's threads section: one line per I/O thread, numbered from 1 since
// the main thread is thread 0.
void AppendThreadsSection(const IoThreadList* const threads,
                          std::string* const info) {
  info->append("# Threads\r\n");
  if (threads == nullptr) {
    return;
  }
  for (const std::unique_ptr<IoThread>& thread : *threads) {
    const IoThreadInfo thread_info = thread->Info();
    info->append("io_thread_")
        .append(std::to_string(thread->Index()))
        .append(":clients=")
        .append(std::to_string(thread_info.clients))
        .append(",reads_processed=")
        .append(std::to_string(thread_info.reads_processed))
        .append(",writes_processed=")
        .append(std::to_string(thread_info.writes_processed))
        .append("\r\n");
  }
}
}  // namespace

void HandleInfo(Client* const client) {
  const command::CommandArgs& args = client->Args();
  const bool all = args.empty() || utils::EqualsIgnoreCase(args[0], "all") ||
                   utils::EqualsIgnoreCase(args[0], "default");
  const bool persistence =
      all || utils::EqualsIgnoreCase(args[0], "persistence");
  const bool stats = all || utils::EqualsIgnoreCase(args[0], "stats");
  const bool threads = all || utils::EqualsIgnoreCase(args[0], "threads");
  std::string info;
  if (persistence) {
    AppendPersistenceSection(client->Aof(), &info);
  }
  if (stats) {
    if (!info.empty()) {
      info.append("\r\n");
    }
    AppendStatsSection(client->IoThreads(), &info);
  }
  if (threads) {
    if (!info.empty()) {
      info.append("\r\n");
    }
    AppendThreadsSection(client->IoThreads(), &info);
  }
  client->AddBulkReply(info);
}
}  // namespace redis_simple::command::persistence
