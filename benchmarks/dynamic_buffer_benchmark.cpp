#include <benchmark/benchmark.h>

#include <random>

#include "benchmarks/buffer.h"
#include "memory/dynamic_buffer.h"

namespace redis_simple {
namespace {
memory::DynamicBuffer& DynamicBufferBenchmarkBuffer() {
  static memory::DynamicBuffer buffer;
  return buffer;
}

void FillBenchmarkBuffer() {
  static std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> letter_dist(0, 25);
  std::uniform_int_distribution<int> newline_dist(0, 1023);
  for (char& i : g_buffer) {
    i = newline_dist(rng) == 0 ? '\n'
                               : static_cast<char>(letter_dist(rng) + 'a');
  }
}

void DynamicBufferWrite(benchmark::State& state) {
  FillBenchmarkBuffer();
  for (auto _ : state) {
    (void)_;
    DynamicBufferBenchmarkBuffer().Append(g_buffer.data(), g_buffer.size());
  }
}

void DynamicBufferProcessAndTrim(benchmark::State& state) {
  for (auto _ : state) {
    (void)_;
    const std::string s = DynamicBufferBenchmarkBuffer().ReadLine();
    benchmark::DoNotOptimize(s.data());
    benchmark::DoNotOptimize(s.size());
  }
  DynamicBufferBenchmarkBuffer().Compact();
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(DynamicBufferWrite);
BENCHMARK(DynamicBufferProcessAndTrim);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
