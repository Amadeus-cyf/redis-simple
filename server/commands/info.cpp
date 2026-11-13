#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "server/aof/aof.h"
#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/io_thread.h"
#include "server/rdb/rdb.h"
#include "utils/string_utils.h"

// INFO, with the sections this server reports, in Redis' field names and
// order: persistence (RDB snapshots and the AOF), stats (threaded I/O
// totals), and threads.
namespace redis_simple::commands::server_info {
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

void AppendField(std::string_view name, int64_t value,
                 std::string* const output) {
  AppendField(name, std::to_string(value), output);
}

void AppendRdbFields(const rdb::Rdb* const snapshots, std::string* const info) {
  const rdb::RdbState state =
      snapshots != nullptr ? snapshots->State() : rdb::RdbState{};
  AppendField("rdb_changes_since_last_save", state.changes_since_last_save,
              info);
  AppendField("rdb_bgsave_in_progress",
              state.background_save_in_progress ? "1" : "0", info);
  AppendField("rdb_last_save_time", state.last_save_time, info);
  AppendField("rdb_last_bgsave_status",
              state.last_background_save_ok ? "ok" : "err", info);
  AppendField("rdb_last_bgsave_time_sec", state.last_background_save_seconds,
              info);
  AppendField("rdb_current_bgsave_time_sec",
              state.current_background_save_seconds, info);
  AppendField("rdb_saves", state.saves, info);
  AppendField("rdb_saves_consecutive_failures", state.consecutive_failures,
              info);
  AppendField("rdb_last_load_keys_expired", state.last_load_keys_expired, info);
  AppendField("rdb_last_load_keys_loaded", state.last_load_keys_loaded, info);
}

void AppendPersistenceSection(const rdb::Rdb* const snapshots,
                              const aof::Aof* const append_only_file,
                              std::string* const info) {
  // Redis' fields, in its order, without the copy-on-write sizes; the AOF
  // size fields only when the AOF is on. The server answers no command while
  // it loads, so loading is always 0.
  info->append("# Persistence\r\n");
  AppendField("loading", "0", info);
  AppendField("async_loading", "0", info);
  AppendRdbFields(snapshots, info);
  const aof::AofState state =
      append_only_file != nullptr ? append_only_file->State() : aof::AofState{};
  AppendField("aof_enabled", append_only_file == nullptr ? "0" : "1", info);
  AppendField("aof_rewrite_in_progress", state.rewrite_in_progress ? "1" : "0",
              info);
  AppendField("aof_rewrite_scheduled", state.rewrite_scheduled ? "1" : "0",
              info);
  AppendField("aof_last_rewrite_time_sec", state.last_rewrite_seconds, info);
  AppendField("aof_current_rewrite_time_sec", state.current_rewrite_seconds,
              info);
  AppendField("aof_last_bgrewrite_status", state.last_rewrite_ok ? "ok" : "err",
              info);
  AppendField("aof_rewrites", state.rewrites, info);
  AppendField("aof_rewrites_consecutive_failures",
              state.consecutive_rewrite_failures, info);
  AppendField(
      "aof_last_write_status",
      append_only_file == nullptr || state.accepting_writes ? "ok" : "err",
      info);
  if (append_only_file == nullptr) {
    return;
  }
  AppendField("aof_current_size", state.current_size, info);
  AppendField("aof_base_size", state.base_size, info);
  AppendField("aof_pending_rewrite", state.rewrite_scheduled ? "1" : "0", info);
  AppendField("aof_buffer_length", state.buffer_length, info);
  AppendField("aof_pending_bio_fsync", state.pending_fsyncs, info);
  AppendField("aof_delayed_fsync", state.delayed_fsyncs, info);
}

using IoThreadList = std::span<const std::unique_ptr<IoThread>>;

// Threaded I/O fields of Redis' stats section: totals over the I/O threads.
void AppendStatsSection(IoThreadList threads, std::string* const info) {
  uint64_t reads = 0;
  uint64_t writes = 0;
  for (const std::unique_ptr<IoThread>& thread : threads) {
    const IoThreadInfo thread_info = thread->Info();
    reads += thread_info.reads_processed;
    writes += thread_info.writes_processed;
  }
  const bool active = !threads.empty();
  info->append("# Stats\r\n");
  AppendField("io_threads_active", active ? "1" : "0", info);
  AppendField("io_threaded_reads_processed", static_cast<size_t>(reads), info);
  AppendField("io_threaded_writes_processed", static_cast<size_t>(writes),
              info);
}

// Redis 8's threads section: one line per I/O thread, numbered from 1 since
// the main thread is thread 0.
void AppendThreadsSection(IoThreadList threads, std::string* const info) {
  info->append("# Threads\r\n");
  for (const std::unique_ptr<IoThread>& thread : threads) {
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
  // Redis' genInfoSectionDict: no argument asks for every section, and so
  // do "all", "default", and "everything", which differ only in sections
  // this server does not have. Any number of sections may be named; each is
  // reported once, in Redis' order, and unknown names are ignored.
  const commands::CommandArgs& args = client->Args();
  bool persistence = args.empty();
  bool stats = args.empty();
  bool threads = args.empty();
  for (const std::string_view section : args) {
    const bool all = utils::EqualsIgnoreCase(section, "all") ||
                     utils::EqualsIgnoreCase(section, "default") ||
                     utils::EqualsIgnoreCase(section, "everything");
    persistence =
        persistence || all || utils::EqualsIgnoreCase(section, "persistence");
    stats = stats || all || utils::EqualsIgnoreCase(section, "stats");
    threads = threads || all || utils::EqualsIgnoreCase(section, "threads");
  }
  std::string info;
  if (persistence) {
    AppendPersistenceSection(client->Server().rdb, client->Aof(), &info);
  }
  if (stats) {
    if (!info.empty()) {
      info.append("\r\n");
    }
    AppendStatsSection(client->Server().io_threads, &info);
  }
  if (threads) {
    if (!info.empty()) {
      info.append("\r\n");
    }
    AppendThreadsSection(client->Server().io_threads, &info);
  }
  client->AddBulkReply(info);
}
}  // namespace redis_simple::commands::server_info
