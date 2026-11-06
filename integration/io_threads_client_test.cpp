// Many concurrent pipelined clients, so a server started with --io-threads
// receives batches large enough to share across its I/O threads. Every reply
// is checked, then the totals and INFO stats.
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cli/cli.h"
#include "logging/logger.h"

namespace redis_simple {
namespace {
constexpr int kPort = 8080;
constexpr size_t kClients = 32;
constexpr size_t kRounds = 150;
// Every this many rounds a client sets and reads a value large enough that
// its reply needs several writes.
constexpr size_t kLargeValueInterval = 50;
constexpr size_t kLargeValueBytes = size_t{128} * 1024;

struct Expected {
  std::vector<std::string_view> command;
  std::string reply;
};

// Sends one round of commands as a single pipeline, then checks each reply.
bool RunRound(cli::RedisCli* const cli, size_t client, size_t round,
              const std::string& large_value) {
  const std::string suffix =
      std::to_string(client) + ":" + std::to_string(round);
  const std::string key = "io:key:" + suffix;
  const std::string value = "value:" + suffix;
  const std::string counter = "io:counter:" + std::to_string(client);
  const std::string list = "io:list:" + std::to_string(client);
  const std::string count = std::to_string(round + 1);
  const std::string large_key = "io:large:" + std::to_string(client);

  std::vector<Expected> steps = {
      {{"SET", key, value}, "OK\n"},     {{"GET", key}, value + "\n"},
      {{"INCR", counter}, count + "\n"}, {{"RPUSH", list, count}, count + "\n"},
      {{"INCR", "io:shared"}, ""},
  };
  if (round % kLargeValueInterval == 0) {
    steps.push_back({{"SET", large_key, large_value}, "OK\n"});
    steps.push_back({{"GET", large_key}, large_value + "\n"});
  }
  for (const Expected& step : steps) {
    cli->AddCommand(step.command);
  }
  for (const Expected& step : steps) {
    const std::string actual = cli->ReadReply();
    // The shared counter's value depends on the other clients.
    const bool matches =
        step.reply.empty()
            ? !actual.empty() && actual.back() == '\n' &&
                  actual.find_first_not_of("0123456789\n") == std::string::npos
            : actual == step.reply;
    if (!matches) {
      RS_LOG_WARN("client %zu round %zu: %.*s got %.80s\n", client, round,
                  static_cast<int>(step.command.front().size()),
                  step.command.front().data(), actual.c_str());
      return false;
    }
  }
  return true;
}

bool RunClients() {
  const std::string large_value(kLargeValueBytes, 'L');
  std::atomic<size_t> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kClients);
  for (size_t client = 0; client < kClients; ++client) {
    threads.emplace_back([&failures, &large_value, client] {
      cli::RedisCli cli;
      if (cli.Connect("localhost", kPort) == cli::CliStatus::kError) {
        failures.fetch_add(1);
        return;
      }
      for (size_t round = 0; round < kRounds; ++round) {
        if (!RunRound(&cli, client, round, large_value)) {
          failures.fetch_add(1);
          return;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return failures.load() == 0;
}

bool ExpectReply(cli::RedisCli* const cli,
                 const std::vector<std::string_view>& command,
                 std::string_view expected) {
  cli->AddCommand(command);
  const std::string actual = cli->ReadReply();
  if (actual == expected) {
    return true;
  }
  RS_LOG_WARN("%.*s: expected %.*s, got %s\n",
              static_cast<int>(command.front().size()), command.front().data(),
              static_cast<int>(expected.size()), expected.data(),
              actual.c_str());
  return false;
}

// Returns the value of an integer field in an INFO reply, or -1.
int64_t InfoField(std::string_view info, std::string_view field) {
  const std::string prefix = std::string(field) + ":";
  const size_t start = info.find(prefix);
  if (start == std::string_view::npos) {
    return -1;
  }
  const std::string_view rest = info.substr(start + prefix.size());
  int64_t value = -1;
  std::from_chars(rest.data(), rest.data() + rest.size(), value);
  return value;
}
}  // namespace

int Run(bool expect_threads) {
  if (!RunClients()) {
    return EXIT_FAILURE;
  }
  cli::RedisCli cli;
  if (cli.Connect("localhost", kPort) == cli::CliStatus::kError) {
    return EXIT_FAILURE;
  }
  if (!ExpectReply(&cli, {"GET", "io:shared"},
                   std::to_string(kClients * kRounds) + "\n")) {
    return EXIT_FAILURE;
  }
  for (size_t client = 0; client < kClients; ++client) {
    if (!ExpectReply(&cli, {"LLEN", "io:list:" + std::to_string(client)},
                     std::to_string(kRounds) + "\n")) {
      return EXIT_FAILURE;
    }
  }
  cli.AddCommand(std::vector<std::string_view>{"INFO", "stats"});
  const std::string stats = cli.ReadReply();
  const int64_t reads = InfoField(stats, "io_threaded_reads_processed");
  const int64_t writes = InfoField(stats, "io_threaded_writes_processed");
  if (reads < 0 || writes < 0 ||
      (expect_threads && (reads == 0 || writes == 0)) ||
      (!expect_threads && (reads != 0 || writes != 0))) {
    RS_LOG_WARN("unexpected threaded I/O counters: %s\n", stats.c_str());
    return EXIT_FAILURE;
  }
  return ExpectReply(&cli, {"FLUSHDB"}, "OK\n") ? EXIT_SUCCESS : EXIT_FAILURE;
}
}  // namespace redis_simple

int main() {
  // The runner script passes server flags through this variable, so the
  // test knows whether the server has I/O threads to use.
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
  const char* const server_args = std::getenv("REDIS_SIMPLE_SERVER_ARGS");
  const bool expect_threads =
      server_args != nullptr &&
      std::string_view(server_args).contains("--io-threads") &&
      !std::string_view(server_args).contains("--io-threads 1");
  try {
    return redis_simple::Run(expect_threads);
  } catch (...) {
    return EXIT_FAILURE;
  }
}
