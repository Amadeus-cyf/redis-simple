#include <benchmark/benchmark.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "logging/logger.h"
#include "server/aof/aof.h"
#include "server/db/db.h"
#include "server/reply.h"

// AOF load benchmarks: what a restart pays to read the file back.
namespace redis_simple {
namespace {
// A temporary AOF directory whose manifest lists one incremental file with
// the given commands, removed afterwards.
class AofDirectory {
 public:
  explicit AofDirectory(std::string_view commands) {
    constexpr std::string_view kTemplate =
        "/tmp/redis_simple_aof_benchmark_XXXXXX";
    std::array<char, kTemplate.size() + 1> path{};
    std::ranges::copy(kTemplate, path.begin());
    if (mkdtemp(path.data()) == nullptr) {
      return;
    }
    root_ = path.data();
    const std::string directory = root_ + "/appendonlydir";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream manifest(directory + "/appendonly.aof.manifest",
                           std::ios::binary);
    manifest << "file appendonly.aof.1.incr.aof seq 1 type i\n";
    std::ofstream file(directory + "/appendonly.aof.1.incr.aof",
                       std::ios::binary);
    file.write(commands.data(), static_cast<std::streamsize>(commands.size()));
    manifest.close();
    file.close();
    written_ = !error && !manifest.fail() && !file.fail();
  }
  AofDirectory(const AofDirectory&) = delete;
  AofDirectory& operator=(const AofDirectory&) = delete;
  AofDirectory(AofDirectory&&) = delete;
  AofDirectory& operator=(AofDirectory&&) = delete;
  ~AofDirectory() {
    if (!root_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(root_, error);
    }
  }

  [[nodiscard]] bool Written() const { return written_; }
  // Without an fsync of its own, so that a load is all an open does.
  [[nodiscard]] aof::Options Options() const {
    return {root_ + "/appendonlydir", "appendonly.aof", aof::FsyncPolicy::kNo};
  }

 private:
  std::string root_;
  bool written_{};
};

// Opens the AOF that holds `commands` into an empty database, once per
// iteration, and checks that `key` was loaded. Closing the AOF and freeing
// the database are not timed.
void LoadAof(benchmark::State& state, std::string_view commands,
             std::string_view key) {
  const AofDirectory directory(commands);
  if (!directory.Written()) {
    state.SkipWithError("could not write the AOF directory");
    return;
  }
  // An open logs each file it loads.
  const logging::Level level = logging::MinLevel();
  logging::SetLevel(logging::Level::kWarn);
  for (auto _ : state) {
    (void)_;
    auto database = db::RedisDb::Create();
    auto aof = aof::Aof::Open(directory.Options(), database.get());
    if (aof == nullptr || database->LookupKey(key) == nullptr) {
      state.SkipWithError("the AOF did not load");
      break;
    }
    state.PauseTiming();
    aof.reset();
    database.reset();
    state.ResumeTiming();
  }
  logging::SetLevel(level);
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(commands.size()));
}

// Loads a file that holds one `RPUSH list <value>...` of eight-byte values,
// 14 bytes each in the file. The loader reads 64 KiB at a time, so the
// command reaches it in one read at 1,000 values, in 3 at 10,000, in 22 at
// 100,000 and in 214 at 1,000,000.
void AofLoadLargeCommand(benchmark::State& state) {
  const auto values = static_cast<size_t>(state.range(0));
  std::string command = reply::FromArrayHeader(values + 2);
  reply::AppendBulkString("RPUSH", &command);
  reply::AppendBulkString("list", &command);
  for (size_t index = 0; index < values; ++index) {
    std::string value = std::to_string(index);
    value.insert(0, 8 - value.size(), '0');
    reply::AppendBulkString(value, &command);
  }
  LoadAof(state, command, "list");
}

// Loads a file of `SET key:<n> value` commands, each far shorter than a
// read, for comparison.
void AofLoadSmallCommands(benchmark::State& state) {
  const auto count = static_cast<size_t>(state.range(0));
  std::string commands;
  for (size_t index = 0; index < count; ++index) {
    commands.append(reply::FromArrayHeader(3));
    reply::AppendBulkString("SET", &commands);
    reply::AppendBulkString("key:" + std::to_string(index), &commands);
    reply::AppendBulkString("value", &commands);
  }
  LoadAof(state, commands, "key:0");
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(AofLoadLargeCommand)
    ->ArgName("values")
    ->Arg(1'000)
    ->Arg(10'000)
    ->Arg(100'000)
    ->Arg(1'000'000)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(AofLoadSmallCommands)
    ->ArgName("commands")
    ->Arg(100'000)
    ->Unit(benchmark::kMicrosecond);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
