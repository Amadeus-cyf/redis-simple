#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "containers/reply_buffer.h"
#include "server/reply.h"
#include "utils/floats.h"

// Reply encoding benchmarks: collection replies such as LRANGE, HGETALL, and
// ZRANGE WITHSCORES, which encode one bulk string or score per element, and
// the single integer and float replies of commands such as INCR and ZSCORE.
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

// Encodes an array reply of short bulk strings. With `reserve`, the reply's
// room is reserved from the element count first, as commands that know the
// count do.
void AppendBulkStrings(benchmark::State& state) {
  const auto values = Values(static_cast<size_t>(state.range(0)));
  const bool reserve = state.range(1) != 0;
  for (auto _ : state) {
    (void)_;
    std::string reply = reply::FromArrayHeader(values.size());
    if (reserve) {
      reply::ReserveBulkStrings(values.size(), &reply);
    }
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

// Queues `reply` and sends it, as a client with one pending reply does.
void SendReply(std::string_view reply, containers::ReplyBuffer* const buffer) {
  buffer->Append(reply.data(), reply.size());
  benchmark::DoNotOptimize(buffer->PendingBytes());
  buffer->Consume(buffer->PendingBytes());
}

// Encodes one integer reply on the stack and queues it, as
// Client::AddIntegerReply does for values other than 0 and 1.
void IntegerReply(benchmark::State& state) {
  int64_t value = state.range(0);
  containers::ReplyBuffer buffer;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(value);
    reply::IntegerBuffer encoded;
    SendReply(reply::EncodeInt64(value, &encoded), &buffer);
  }
  state.SetItemsProcessed(state.iterations());
}

// Queues the shared reply for 1, as Client::AddIntegerReply does for 0 and 1.
void SharedIntegerReply(benchmark::State& state) {
  containers::ReplyBuffer buffer;
  for (auto _ : state) {
    (void)_;
    SendReply(reply::kOne, &buffer);
  }
  state.SetItemsProcessed(state.iterations());
}

// Encodes one score reply on the stack and queues it, as
// Client::AddFloatReply does for ZSCORE and ZINCRBY.
void FloatReply(benchmark::State& state) {
  double value = state.range(0) != 0 ? 1234.5678 : 1234.0;
  const auto protocol = state.range(1) == 3 ? reply::ProtocolVersion::kResp3
                                            : reply::ProtocolVersion::kResp2;
  containers::ReplyBuffer buffer;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(value);
    reply::FloatReplyBuffer encoded;
    SendReply(reply::EncodeFloat(value, protocol, &encoded), &buffer);
  }
  state.SetItemsProcessed(state.iterations());
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(AppendBulkStrings)
    ->ArgsProduct({{100, 1000}, {0, 1}})
    ->ArgNames({"elements", "reserve"});
BENCHMARK(FormatScore)->Arg(0)->Arg(1)->ArgName("fractional");
BENCHMARK(AppendScores)->Arg(0)->Arg(1)->ArgName("fractional");
BENCHMARK(IntegerReply)
    ->Arg(7)
    ->Arg(1234567)
    ->Arg(std::numeric_limits<int64_t>::min());
BENCHMARK(SharedIntegerReply);
BENCHMARK(FloatReply)
    ->ArgsProduct({{0, 1}, {2, 3}})
    ->ArgNames({"fractional", "protocol"});
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
