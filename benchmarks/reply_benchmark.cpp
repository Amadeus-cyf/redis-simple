#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "server/reply.h"
#include "utils/float_utils.h"

// Reply encoding benchmarks for collection replies such as LRANGE, HGETALL,
// and ZRANGE WITHSCORES, which encode one bulk string or score per element.
namespace redis_simple {
namespace {
std::vector<std::string> Values(size_t count) {
  std::vector<std::string> values;
  values.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    values.push_back("value:" + std::to_string(i));
  }
  return values;
}

// Encodes an array reply of short bulk strings.
void AppendBulkStrings(benchmark::State& state) {
  const auto values = Values(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    (void)_;
    std::string reply = reply::FromArrayHeader(values.size());
    for (const std::string& value : values) {
      reply::AppendBulkString(value, &reply);
    }
    benchmark::DoNotOptimize(reply.data());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Formats a score. Argument 0 uses whole numbers, which take the integer
// fast path; argument 1 uses fractional scores, which take std::to_chars.
void FormatScore(benchmark::State& state) {
  const bool fractional = state.range(0) != 0;
  std::vector<double> scores;
  scores.reserve(1024);
  for (int i = 0; i < 1024; ++i) {
    scores.push_back(fractional ? (i * 1.25) + 0.1 : i * 1000.0);
  }
  size_t next = 0;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(utils::FloatToString(scores[next]));
    next = (next + 1) % scores.size();
  }
  state.SetItemsProcessed(state.iterations());
}

// Encodes ZRANGE WITHSCORES scores straight into a RESP2 reply.
void AppendScores(benchmark::State& state) {
  const bool fractional = state.range(0) != 0;
  std::vector<double> scores;
  scores.reserve(100);
  for (int i = 0; i < 100; ++i) {
    scores.push_back(fractional ? (i * 3.0) + 0.25 : i * 3.0);
  }
  for (auto _ : state) {
    (void)_;
    std::string reply;
    for (const double score : scores) {
      reply::AppendFloat(score, reply::ProtocolVersion::kResp2, &reply);
    }
    benchmark::DoNotOptimize(reply.data());
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(scores.size()));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(AppendBulkStrings)->Arg(100)->Arg(1000);
BENCHMARK(FormatScore)->Arg(0)->Arg(1)->ArgName("fractional");
BENCHMARK(AppendScores)->Arg(0)->Arg(1)->ArgName("fractional");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
