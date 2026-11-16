#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "server/commands/command.h"
#include "server/multi_state.h"

// MULTI queueing benchmarks: the copy every command inside a transaction
// pays, and the memory a client keeps once the transaction ends.
namespace redis_simple {
namespace {
// Queues commands as a client does between MULTI and EXEC, then ends the
// transaction. The arguments are the number of commands and the value size:
// each command is `SET key:<n> <value>`, or a lone `PING` when the value size
// is zero, which leaves the queue's bookkeeping far larger than its argument
// bytes. `retained_bytes` is what the queue still holds afterwards, and
// `queued_bytes` what it held before the transaction ended.
void MultiQueue(benchmark::State& state) {
  const auto command_count = static_cast<size_t>(state.range(0));
  const std::string value(static_cast<size_t>(state.range(1)), 'v');
  const commands::Command* const command =
      commands::Find(value.empty() ? "PING" : "SET");
  std::vector<std::string> keys;
  keys.reserve(command_count);
  for (size_t i = 0; i < command_count; ++i) {
    keys.push_back("key:" + std::to_string(i));
  }
  MultiState multi;
  commands::CommandArgs args;
  size_t queued_bytes = 0;
  for (auto _ : state) {
    (void)_;
    multi.Begin();
    for (const std::string& key : keys) {
      if (value.empty()) {
        args.assign({"PING"});
      } else {
        args.assign({"SET", key, value});
      }
      benchmark::DoNotOptimize(multi.Queue(command, args));
    }
    queued_bytes = multi.AllocatedBytes();
    multi.Reset();
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(command_count));
  state.counters["queued_bytes"] = static_cast<double>(queued_bytes);
  state.counters["retained_bytes"] =
      static_cast<double>(multi.AllocatedBytes());
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(MultiQueue)
    ->ArgNames({"commands", "value_size"})
    ->Args({10, 3})
    ->Args({10'000, 0})
    ->Args({10'000, 3})
    ->Args({10'000, 100})
    ->Args({10'000, 4'096})
    ->Unit(benchmark::kMicrosecond);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
