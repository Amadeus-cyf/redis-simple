#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cli/cli.h"
#include "integration/client_helpers.h"
#include "logging/logger.h"

namespace redis_simple {
namespace {
using integration::Case;
using integration::NonEmptyLines;

constexpr int kRdbPort = 18082;

// Compares line by line, since the client ends arrays with a blank line.
bool ExpectReplyLines(cli::RedisCli* const cli, const Case& test_case) {
  cli->AddCommand(test_case.command);
  const std::string reply = cli->ReadReply();
  if (NonEmptyLines(reply) == NonEmptyLines(test_case.expected_reply)) {
    return true;
  }
  RS_LOG_DEBUG("RDB command failed: %s expected: %s actual: %s\n",
               test_case.command.c_str(), test_case.expected_reply.c_str(),
               reply.c_str());
  return false;
}

bool ExpectReplies(cli::RedisCli* const cli, const std::vector<Case>& cases) {
  return std::ranges::all_of(cases, [cli](const Case& test_case) {
    return ExpectReplyLines(cli, test_case);
  });
}

// LASTSAVE's reply, as a number.
int64_t LastSave(cli::RedisCli* const cli) {
  cli->AddCommand("LASTSAVE\r\n");
  const std::string reply = cli->ReadReply();
  return std::strtoll(reply.c_str(), nullptr, 10);
}

// A field of INFO persistence, or an empty string if it is missing.
std::string InfoField(cli::RedisCli* const cli, std::string_view name) {
  cli->AddCommand("INFO persistence\r\n");
  const std::string prefix = std::string(name) + ":";
  for (std::string line : NonEmptyLines(cli->ReadReply())) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.starts_with(prefix)) {
      return line.substr(prefix.size());
    }
  }
  return {};
}

bool ExpectInfo(
    cli::RedisCli* const cli,
    const std::vector<std::pair<std::string_view, std::string>>& fields) {
  return std::ranges::all_of(fields, [cli](const auto& field) {
    const std::string value = InfoField(cli, field.first);
    if (value == field.second) {
      return true;
    }
    RS_LOG_DEBUG("INFO %.*s is %s, expected %s\n",
                 static_cast<int>(field.first.size()), field.first.data(),
                 value.c_str(), field.second.c_str());
    return false;
  });
}

// Polls INFO for up to ten seconds until `field` has `value`.
bool WaitForInfo(cli::RedisCli* const cli, std::string_view field,
                 std::string_view value) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (InfoField(cli, field) == value) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  RS_LOG_DEBUG("INFO %.*s never became %.*s\n", static_cast<int>(field.size()),
               field.data(), static_cast<int>(value.size()), value.data());
  return false;
}

// Fills every type, in small and large encodings, then saves.
bool WriteDataset(cli::RedisCli* const cli) {
  std::vector<Case> cases = {
      {"FLUSHDB\r\n", "OK\n"},
      {"SET rdb_string value\r\n", "OK\n"},
      {"SET rdb_int 42\r\n", "OK\n"},
      {"SET rdb_ttl live PXAT 4102444800000\r\n", "OK\n"},
      {"RPUSH rdb_list a 1 -5 bbb\r\n", "4\n"},
      {"SADD rdb_ints 3 1 -2\r\n", "3\n"},
      {"SADD rdb_strings b a\r\n", "2\n"},
      {"ZADD rdb_zset 1.5 a 2 b -inf c\r\n", "3\n"},
      {"HSET rdb_hash f1 v1 n 12345\r\n", "2\n"},
  };
  for (int i = 0; i < 300; ++i) {
    const std::string number = std::to_string(i);
    cases.push_back({"RPUSH rdb_big_list element-" + number + "\r\n",
                     std::to_string(i + 1) + "\n"});
    cases.push_back({"SADD rdb_big_set member-" + number + "\r\n", "1\n"});
    std::string zadd = "ZADD rdb_big_zset ";
    zadd.append(number).append(" member-").append(number).append("\r\n");
    cases.push_back({std::move(zadd), "1\n"});
    std::string hset = "HSET rdb_big_hash field-";
    hset.append(number).append(" ").append(70, 'v').append("\r\n");
    cases.push_back({std::move(hset), "1\n"});
  }
  if (!ExpectReplies(cli, cases)) {
    return false;
  }
  const int64_t before = LastSave(cli);
  if (!ExpectReplies(cli, {
                              {"SAVE\r\n", "OK\n"},
                              {"SAVE extra\r\n",
                               "ERR wrong number of arguments for 'save' "
                               "command\n"},
                              // SAVE is not allowed inside MULTI, which then
                              // aborts.
                              {"MULTI\r\n", "OK\n"},
                              {"SET rdb_not_saved v\r\n", "QUEUED\n"},
                              {"SAVE\r\n",
                               "ERR Command not allowed inside a "
                               "transaction\n"},
                              {"EXEC\r\n",
                               "EXECABORT Transaction discarded because of "
                               "previous errors.\n"},
                          })) {
    return false;
  }
  // LASTSAVE reports the start time before the first save, then the save.
  if (LastSave(cli) < before || before <= 0) {
    RS_LOG_DEBUG("LASTSAVE did not advance\n");
    return false;
  }
  // Written after the save, so the restart must not see it.
  return ExpectReplyLines(cli, {"SET rdb_after_save lost\r\n", "OK\n"});
}

bool ReadDataset(cli::RedisCli* const cli) {
  std::vector<Case> cases = {
      {"GET rdb_string\r\n", "value\n"},
      {"GET rdb_int\r\n", "42\n"},
      {"GET rdb_ttl\r\n", "live\n"},
      {"EXISTS rdb_after_save\r\n", "0\n"},
      {"EXISTS rdb_not_saved\r\n", "0\n"},
      {"LRANGE rdb_list 0 -1\r\n", "a\n1\n-5\nbbb\n"},
      {"SISMEMBER rdb_ints -2\r\n", "1\n"},
      {"SCARD rdb_ints\r\n", "3\n"},
      {"SCARD rdb_strings\r\n", "2\n"},
      {"ZRANGE rdb_zset 0 -1 WITHSCORES\r\n", "c\n-inf\na\n1.5\nb\n2\n"},
      {"HGET rdb_hash n\r\n", "12345\n"},
      {"LLEN rdb_big_list\r\n", "300\n"},
      {"LINDEX rdb_big_list -1\r\n", "element-299\n"},
      {"SCARD rdb_big_set\r\n", "300\n"},
      {"ZSCORE rdb_big_zset member-299\r\n", "299\n"},
      {"HLEN rdb_big_hash\r\n", "300\n"},
      {"HGET rdb_big_hash field-7\r\n", std::string(70, 'v') + "\n"},
      {"DBSIZE\r\n", "12\n"},
  };
  if (!ExpectReplies(cli, cases)) {
    return false;
  }
  cli->AddCommand("PTTL rdb_ttl\r\n");
  const std::vector<std::string> ttl = NonEmptyLines(cli->ReadReply());
  return ttl.size() == 1 && std::strtoll(ttl[0].c_str(), nullptr, 10) > 0;
}

bool ExpectEmpty(cli::RedisCli* const cli) {
  return ExpectReplyLines(cli, {"DBSIZE\r\n", "0\n"});
}

using InfoFields = std::vector<std::pair<std::string_view, std::string>>;

// BGSAVE, its scheduled form, and the INFO fields, in a server started
// without a file, with the default save points.
bool BackgroundSave(cli::RedisCli* const cli) {
  // Changes count as Redis counts them: one per key set, per member added,
  // and per field pair given, even when the field already existed.
  const std::vector<Case> writes = {
      {"SET bg_key v\r\n", "OK\n"},
      {"SADD bg_set a b c\r\n", "3\n"},
      {"HSET bg_hash f 1 f 2\r\n", "1\n"},
  };
  const InfoFields before_save = {
      {"rdb_changes_since_last_save", "6"},  {"rdb_bgsave_in_progress", "0"},
      {"rdb_last_bgsave_status", "ok"},      {"rdb_last_bgsave_time_sec", "-1"},
      {"rdb_current_bgsave_time_sec", "-1"}, {"rdb_saves", "0"},
      {"rdb_last_load_keys_loaded", "0"}};
  if (!ExpectReplies(cli, writes) || !ExpectInfo(cli, before_save)) {
    return false;
  }
  const int64_t before = LastSave(cli);
  const std::vector<Case> background_save = {
      {"BGSAVE now\r\n", "ERR syntax error\n"},
      {"BGSAVE\r\n", "Background saving started\n"},
  };
  const InfoFields after_save = {{"rdb_changes_since_last_save", "0"},
                                 {"rdb_last_bgsave_status", "ok"},
                                 {"rdb_saves", "1"},
                                 {"rdb_saves_consecutive_failures", "0"}};
  if (!ExpectReplies(cli, background_save) ||
      !WaitForInfo(cli, "rdb_bgsave_in_progress", "0") ||
      !ExpectInfo(cli, after_save) || LastSave(cli) < before) {
    return false;
  }
  // Inside EXEC the save is scheduled, and the cron starts it.
  const std::vector<Case> scheduled = {
      {"MULTI\r\n", "OK\n"},
      {"BGSAVE\r\n", "QUEUED\n"},
      {"EXEC\r\n", "Background saving scheduled\n"},
  };
  if (!ExpectReplies(cli, scheduled) || !WaitForInfo(cli, "rdb_saves", "2") ||
      !WaitForInfo(cli, "rdb_bgsave_in_progress", "0")) {
    return false;
  }
  // Not saved by BGSAVE, so only the save at shutdown keeps it.
  return ExpectReplyLines(cli, {"SET bg_after_save kept\r\n", "OK\n"});
}

// After a restart: the save at shutdown kept the last write.
bool SavedAtShutdown(cli::RedisCli* const cli) {
  const std::vector<Case> reads = {
      {"GET bg_after_save\r\n", "kept\n"},
      {"SCARD bg_set\r\n", "3\n"},
      {"HGET bg_hash f\r\n", "2\n"},
  };
  const InfoFields loaded = {{"rdb_last_load_keys_loaded", "4"},
                             {"rdb_last_load_keys_expired", "0"},
                             {"rdb_changes_since_last_save", "0"}};
  return ExpectReplies(cli, reads) && ExpectInfo(cli, loaded);
}

// With --save "1 1", one change starts a background save a second later.
bool SavePoint(cli::RedisCli* const cli) {
  const InfoFields saved = {{"rdb_changes_since_last_save", "0"},
                            {"rdb_last_bgsave_status", "ok"}};
  const Case write = {"SET sp_key v\r\n", "OK\n"};
  return ExpectReplyLines(cli, write) && WaitForInfo(cli, "rdb_saves", "1") &&
         WaitForInfo(cli, "rdb_bgsave_in_progress", "0") &&
         ExpectInfo(cli, saved);
}

// A server whose directory was deleted: the save point's save fails, and
// with stop-writes-on-bgsave-error writes and PING are refused, as in Redis.
bool RefusesWritesAfterFailedSave(cli::RedisCli* const cli) {
  const std::string misconf =
      "MISCONF Redis is configured to save RDB snapshots, but it's currently "
      "unable to persist to disk. Commands that may modify the data set are "
      "disabled, because this instance is configured to report errors during "
      "writes if RDB snapshotting fails (stop-writes-on-bgsave-error option). "
      "Please check the Redis logs for details about the RDB error.\n";
  const InfoFields failed = {{"rdb_saves_consecutive_failures", "1"}};
  const std::vector<Case> refused = {
      {"SET mc_key w\r\n", misconf},
      {"PING\r\n", misconf},
      {"GET mc_key\r\n", "v\n"},
      {"MULTI\r\n", "OK\n"},
      {"GET mc_key\r\n", "QUEUED\n"},
      {"EXEC\r\n", "v\n"},
      {"MULTI\r\n", "OK\n"},
      {"SET mc_key w\r\n", misconf},
      {"EXEC\r\n",
       "EXECABORT Transaction discarded because of previous errors.\n"},
  };
  const Case write = {"SET mc_key v\r\n", "OK\n"};
  return ExpectReplyLines(cli, write) &&
         WaitForInfo(cli, "rdb_last_bgsave_status", "err") &&
         ExpectInfo(cli, failed) && ExpectReplies(cli, refused);
}

// The same failure with --stop-writes-on-bgsave-error no: writes go on.
bool KeepsWritingAfterFailedSave(cli::RedisCli* const cli) {
  const std::vector<Case> allowed = {
      {"SET mc_key w\r\n", "OK\n"},
      {"PING\r\n", "PONG\n"},
  };
  const Case write = {"SET mc_key v\r\n", "OK\n"};
  return ExpectReplyLines(cli, write) &&
         WaitForInfo(cli, "rdb_last_bgsave_status", "err") &&
         ExpectReplies(cli, allowed);
}

int Run(std::string_view mode) {
  cli::RedisCli cli;
  if (cli.Connect("127.0.0.1", kRdbPort) == cli::CliStatus::kError) {
    return EXIT_FAILURE;
  }
  if (mode == "write") {
    return WriteDataset(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "read") {
    return ReadDataset(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "empty") {
    return ExpectEmpty(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "bgsave") {
    return BackgroundSave(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "shutdown") {
    return SavedAtShutdown(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "savepoint") {
    return SavePoint(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "misconf") {
    return RefusesWritesAfterFailedSave(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "nostop") {
    return KeepsWritingAfterFailedSave(&cli) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return EXIT_FAILURE;
}
}  // namespace
}  // namespace redis_simple

int main(int argc, char* argv[]) {
  try {
    return argc == 2 ? redis_simple::Run(argv[1]) : EXIT_FAILURE;
  } catch (...) {
    return EXIT_FAILURE;
  }
}
