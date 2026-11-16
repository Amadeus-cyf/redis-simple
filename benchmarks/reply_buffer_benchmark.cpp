#include <benchmark/benchmark.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "containers/reply_buffer.h"

// Reply buffer benchmarks, using ReplyBuffer as Client does: replies are
// appended, gathered into iovecs, and consumed once written. Each benchmark
// owns its buffer, so memory stays bounded and results do not depend on the
// run order.
namespace redis_simple {
namespace {
// `count` replies of `bytes` each, then one write that sends them all: a
// batch of pipelined replies. Batches past the inline buffer spill into the
// reply list.
void ReplyBufferAppendAndWrite(benchmark::State& state) {
  const auto count = static_cast<size_t>(state.range(0));
  const auto bytes = static_cast<size_t>(state.range(1));
  const std::string reply(bytes, 'r');
  containers::ReplyBuffer buffer;
  std::vector<iovec> blocks;
  for (auto _ : state) {
    (void)_;
    for (size_t i = 0; i < count; ++i) {
      buffer.Append(reply.data(), reply.size());
    }
    blocks.clear();
    buffer.FillBlocks(&blocks);
    benchmark::DoNotOptimize(blocks.data());
    buffer.Consume(buffer.PendingBytes());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
  state.SetBytesProcessed(state.iterations() * state.range(0) * state.range(1));
}

// A large batch written in 16 KiB pieces, as a socket that accepts only part
// of the output at a time does.
void ReplyBufferPartialWrites(benchmark::State& state) {
  constexpr size_t kReplies = 256;
  constexpr size_t kReplyBytes = 1024;
  constexpr size_t kWriteBytes = size_t{16} * 1024;
  const std::string reply(kReplyBytes, 'r');
  containers::ReplyBuffer buffer;
  for (auto _ : state) {
    (void)_;
    for (size_t i = 0; i < kReplies; ++i) {
      buffer.Append(reply.data(), reply.size());
    }
    while (!buffer.Empty()) {
      buffer.Consume(kWriteBytes);
    }
  }
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(kReplies * kReplyBytes));
}

// One bulk-string reply of a large value, as GET queues it, then one write.
// Argument 1 queues the value by reference; 0 copies it into reply chunks.
void ReplyBufferLargeBulkReply(benchmark::State& state) {
  const auto bytes = static_cast<size_t>(state.range(0));
  const bool by_reference = state.range(1) != 0;
  const auto value = std::make_shared<const std::string>(bytes, 'v');
  const std::string header = "$" + std::to_string(bytes) + "\r\n";
  containers::ReplyBuffer buffer;
  std::vector<iovec> blocks;
  for (auto _ : state) {
    (void)_;
    buffer.Append(header.data(), header.size());
    if (by_reference) {
      buffer.Append(value);
    } else {
      buffer.Append(value->data(), value->size());
    }
    buffer.Append("\r\n", 2);
    buffer.FillBlocks(&blocks);
    benchmark::DoNotOptimize(blocks.data());
    buffer.Consume(buffer.PendingBytes());
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ReplyBufferAppendAndWrite)
    ->ArgsProduct({{1, 64, 1024}, {8, 512}})
    ->ArgNames({"replies", "bytes"});
BENCHMARK(ReplyBufferPartialWrites);
BENCHMARK(ReplyBufferLargeBulkReply)
    ->ArgsProduct({{65'536, 1'048'576}, {0, 1}})
    ->ArgNames({"bytes", "by_reference"});
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
