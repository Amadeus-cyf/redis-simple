#include <benchmark/benchmark.h>

#include <algorithm>
#include <random>

#include "benchmarks/buffer.h"
#include "memory/reply_buffer.h"

namespace redis_simple {
namespace {
memory::ReplyBuffer& ReplyBufferBenchmarkBuffer() {
  static memory::ReplyBuffer buffer;
  return buffer;
}

std::mt19937& Rng() {
  static std::mt19937 rng(std::random_device{}());
  return rng;
}

void FillBenchmarkBuffer() {
  std::uniform_int_distribution<int> byte_dist(0, 254);
  for (char& i : g_buffer) {
    i = static_cast<char>(byte_dist(Rng()));
  }
}

void ReplyBufferAdd(benchmark::State& state) {
  FillBenchmarkBuffer();
  for (auto _ : state) {
    (void)_;
    ReplyBufferBenchmarkBuffer().Append(g_buffer.data(), g_buffer.size());
  }
}

void ReplyBufferProcess(benchmark::State& state) {
  for (auto _ : state) {
    (void)_;
    std::uniform_int_distribution<size_t> bytes_dist(
        0, ReplyBufferBenchmarkBuffer().ReplyBytes());
    ReplyBufferBenchmarkBuffer().Consume(
        std::min(bytes_dist(Rng()), ReplyBufferBenchmarkBuffer().ReplyBytes()));
  }
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ReplyBufferAdd);
BENCHMARK(ReplyBufferProcess);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
