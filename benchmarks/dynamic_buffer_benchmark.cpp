#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <random>

#include "containers/dynamic_buffer.h"

namespace redis_simple {
namespace {
constexpr size_t kPayloadSize = 4096;

// Leaked, so the static has no destructor to run at exit.
containers::DynamicBuffer& DynamicBufferBenchmarkBuffer() {
  static auto& buffer = *new containers::DynamicBuffer;
  return buffer;
}

// Lower-case letters with a newline about every KiB, like request text.
const std::array<char, kPayloadSize>& Payload() {
  static const std::array<char, kPayloadSize> kPayload = [] {
    std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> letter(0, 25);
    std::uniform_int_distribution<int> newline(0, 1023);
    std::array<char, kPayloadSize> payload{};
    for (char& character : payload) {
      character = newline(generator) == 0
                      ? '\n'
                      : static_cast<char>(letter(generator) + 'a');
    }
    return payload;
  }();
  return kPayload;
}

void DynamicBufferWrite(benchmark::State& state) {
  for (auto _ : state) {
    (void)_;
    DynamicBufferBenchmarkBuffer().Append(Payload().data(), Payload().size());
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
