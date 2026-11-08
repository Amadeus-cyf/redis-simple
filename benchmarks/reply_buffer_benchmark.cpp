#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <random>

#include "memory/reply_buffer.h"

namespace redis_simple {
namespace {
constexpr size_t kPayloadSize = 4096;

// Leaked, so the static has no destructor to run at exit.
memory::ReplyBuffer& ReplyBufferBenchmarkBuffer() {
  static auto& buffer = *new memory::ReplyBuffer;
  return buffer;
}

std::mt19937& Generator() {
  static std::mt19937 generator(std::random_device{}());
  return generator;
}

// Random bytes, as reply payloads may hold any.
const std::array<char, kPayloadSize>& Payload() {
  static const std::array<char, kPayloadSize> kPayload = [] {
    std::uniform_int_distribution<int> byte_value(0, 254);
    std::array<char, kPayloadSize> payload{};
    for (char& byte : payload) {
      byte = static_cast<char>(byte_value(Generator()));
    }
    return payload;
  }();
  return kPayload;
}

void ReplyBufferAdd(benchmark::State& state) {
  for (auto _ : state) {
    (void)_;
    ReplyBufferBenchmarkBuffer().Append(Payload().data(), Payload().size());
  }
}

void ReplyBufferProcess(benchmark::State& state) {
  for (auto _ : state) {
    (void)_;
    std::uniform_int_distribution<size_t> byte_count(
        0, ReplyBufferBenchmarkBuffer().ReplyBytes());
    ReplyBufferBenchmarkBuffer().Consume(std::min(
        byte_count(Generator()), ReplyBufferBenchmarkBuffer().ReplyBytes()));
  }
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ReplyBufferAdd);
BENCHMARK(ReplyBufferProcess);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
