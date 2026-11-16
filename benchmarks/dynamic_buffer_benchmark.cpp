#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "containers/dynamic_buffer.h"

// Query buffer benchmarks, using DynamicBuffer as Client does: each read
// lands in WritableTail and is committed, parsed bytes are consumed, and the
// buffer is compacted after each batch. Each benchmark owns its buffer, so
// memory stays bounded and results do not depend on the run order.
namespace redis_simple {
namespace {
// A read of `read_size` bytes, fully consumed, then compacted: the cycle of
// a client whose requests each arrive in one read.
void DynamicBufferReadConsume(benchmark::State& state) {
  const auto read_size = static_cast<size_t>(state.range(0));
  const std::string payload(read_size, 'r');
  containers::DynamicBuffer buffer;
  for (auto _ : state) {
    (void)_;
    char* const tail = buffer.WritableTail(read_size);
    std::memcpy(tail, payload.data(), read_size);
    buffer.Commit(read_size);
    benchmark::DoNotOptimize(buffer.View().data());
    buffer.Consume(read_size);
    buffer.Compact();
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// A read that leaves part of a request unconsumed, so Compact moves it to
// the front before the next read: a request split across reads.
void DynamicBufferReadWithLeftover(benchmark::State& state) {
  constexpr size_t kReadSize = 4096;
  constexpr size_t kLeftover = 100;
  const std::string payload(kReadSize, 'r');
  containers::DynamicBuffer buffer;
  for (auto _ : state) {
    (void)_;
    char* const tail = buffer.WritableTail(kReadSize);
    std::memcpy(tail, payload.data(), kReadSize);
    buffer.Commit(kReadSize);
    buffer.Consume(buffer.View().size() - kLeftover);
    buffer.Compact();
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kReadSize));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(DynamicBufferReadConsume)->Arg(64)->Arg(4096)->ArgName("bytes");
BENCHMARK(DynamicBufferReadWithLeftover);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
