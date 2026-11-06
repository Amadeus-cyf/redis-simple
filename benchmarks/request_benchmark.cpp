#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "server/commands/command.h"
#include "server/reply.h"
#include "server/request_parser.h"

// Request decoding benchmarks: the parse and dispatch work every command pays
// before its handler runs.
namespace redis_simple {
namespace {
std::string EncodeRequest(const std::vector<std::string_view>& parts) {
  std::string request = reply::FromArrayHeader(parts.size());
  for (const std::string_view part : parts) {
    reply::AppendBulkString(part, &request);
  }
  return request;
}

// Parses one complete request already in the buffer, the common case. The
// argument is the number of elements, from GET up to a 21-element MSET.
void ParseCompleteRequest(benchmark::State& state) {
  const auto element_count = static_cast<size_t>(state.range(0));
  std::vector<std::string> parts = {"MSET"};
  for (size_t i = 1; i < element_count; ++i) {
    parts.push_back("key:" + std::to_string(i));
  }
  const std::string request =
      EncodeRequest(std::vector<std::string_view>(parts.begin(), parts.end()));
  request_parser::ParseState parse_state;
  std::string_view command;
  command::CommandArgs args;
  for (auto _ : state) {
    (void)_;
    request_parser::ParseResult result =
        request_parser::Parse(request, &parse_state, &command, &args);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(args.data());
  }
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(request.size()));
}

// Parses a request that arrives in two reads, split inside its last
// argument, so the parser saves its progress and resumes.
void ParseSplitRequest(benchmark::State& state) {
  const std::string request =
      EncodeRequest({"SET", "key:12345", std::string(64, 'v')});
  const std::string_view first(request.data(), request.size() - 10);
  request_parser::ParseState parse_state;
  std::string_view command;
  command::CommandArgs args;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(
        request_parser::Parse(first, &parse_state, &command, &args));
    benchmark::DoNotOptimize(
        request_parser::Parse(request, &parse_state, &command, &args));
  }
  state.SetItemsProcessed(state.iterations());
}

// Command-table lookup with mixed-case names of various lengths.
void FindCommand(benchmark::State& state) {
  constexpr std::array<std::string_view, 8> kNames = {
      "get",  "SET",           "Incr",   "lpush",
      "HSET", "zrangebyscore", "exists", "BgRewriteAof"};
  size_t next = 0;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(command::Find(kNames[next]));
    next = (next + 1) % kNames.size();
  }
  state.SetItemsProcessed(state.iterations());
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ParseCompleteRequest)->Arg(2)->Arg(3)->Arg(21);
BENCHMARK(ParseSplitRequest);
BENCHMARK(FindCommand);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
