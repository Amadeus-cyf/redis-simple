#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "containers/intset.h"
#include "containers/listpack.h"
#include "data_types/hash/hash.h"
#include "data_types/zset/zset.h"
#include "data_types/zset/zset_range_spec.h"

// Inserts into the compact encodings (intset and listpack), where each insert
// may reallocate the whole value and moves the bytes after it;
// ListpackDeleteFirst measures the same for a delete. Sizes stay within the
// encodings' limits, except that 700 elements make a listpack of about 9 KB,
// slightly more than one 8 KiB quicklist node. ZSetListpackRangeByRank reads
// a range from a listpack, which has to be walked to the range's first member.
namespace redis_simple {
namespace {
// Fixed seeds keep runs comparable, which is the point of these benchmarks.
std::mt19937 SeededRng(uint32_t offset) {
  constexpr uint32_t kSeed = 42;
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  return std::mt19937(kSeed + offset);
}

std::vector<std::string> Names(std::string_view prefix, size_t count) {
  std::vector<std::string> names;
  names.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    names.push_back(std::string(prefix) + std::to_string(i));
  }
  return names;
}

// SADD of ascending integers, each appended at the end of the intset.
void IntsetInsertAscending(benchmark::State& state) {
  const int64_t count = state.range(0);
  for (auto _ : state) {
    (void)_;
    containers::Intset intset;
    for (int64_t value = 0; value < count; ++value) {
      intset.Add(value);
    }
    benchmark::DoNotOptimize(intset.Size());
  }
  state.SetItemsProcessed(state.iterations() * count);
}

// SADD of integers in random order, each moving half the intset on average.
void IntsetInsertShuffled(benchmark::State& state) {
  std::vector<int64_t> values(static_cast<size_t>(state.range(0)));
  std::ranges::iota(values, int64_t{0});
  std::mt19937 generator = SeededRng(0);
  std::ranges::shuffle(values, generator);
  for (auto _ : state) {
    (void)_;
    containers::Intset intset;
    for (const int64_t value : values) {
      intset.Add(value);
    }
    benchmark::DoNotOptimize(intset.Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// RPUSH: every element goes after the last one.
void ListpackAppend(benchmark::State& state) {
  const auto elements = Names("element:", static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    (void)_;
    containers::Listpack listpack;
    for (const auto& element : elements) {
      listpack.Append(element);
    }
    benchmark::DoNotOptimize(listpack.Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// LPUSH: every element goes before the first one, moving all the others.
void ListpackPrepend(benchmark::State& state) {
  const auto elements = Names("element:", static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    (void)_;
    containers::Listpack listpack;
    for (const auto& element : elements) {
      listpack.Prepend(element);
    }
    benchmark::DoNotOptimize(listpack.Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// LPOP until empty, which shrinks the listpack from its largest size.
void ListpackDeleteFirst(benchmark::State& state) {
  const auto elements = Names("element:", static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    containers::Listpack listpack;
    for (const auto& element : elements) {
      listpack.Append(element);
    }
    state.ResumeTiming();
    while (const auto first = listpack.First()) {
      listpack.Delete(*first);
    }
    benchmark::DoNotOptimize(listpack.Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// ZADD of new members with random scores into a listpack-encoded sorted set.
void ZSetListpackInsert(benchmark::State& state) {
  const auto members = Names("member:", static_cast<size_t>(state.range(0)));
  std::mt19937 generator = SeededRng(1);
  std::uniform_real_distribution<double> distribution(0, 1'000'000);
  std::vector<double> scores(members.size());
  std::ranges::generate(scores, [&] { return distribution(generator); });
  for (auto _ : state) {
    (void)_;
    auto zset = data_types::ZSet::Create();
    for (size_t i = 0; i < members.size(); ++i) {
      zset->InsertOrUpdate(members[i], scores[i]);
    }
    benchmark::DoNotOptimize(zset->Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// ZRANGE by rank of ten members from a listpack-encoded sorted set of 128,
// the most a listpack holds, starting at rank `start`. `reverse` reads them
// as ZREVRANGE does, where ranks count from the greatest score. `integral`
// gives the members whole-number scores, which a listpack stores as integers
// and not as text.
void ZSetListpackRangeByRank(benchmark::State& state) {
  constexpr int64_t kRangeSize = 10;
  const int64_t start = state.range(0);
  const bool reverse = state.range(1) != 0;
  const bool integral = state.range(2) != 0;
  const auto members = Names("member:", data_types::ZSet::kListpackMaxEntries);
  std::mt19937 generator = SeededRng(2);
  std::uniform_real_distribution<double> distribution(0, 1'000'000);
  auto zset = data_types::ZSet::Create();
  for (const std::string& member : members) {
    const double score = distribution(generator);
    zset->InsertOrUpdate(member, integral ? std::floor(score) : score);
  }
  if (zset->Encoding() != data_types::ZSet::Encoding::kListpack) {
    state.SkipWithError("the sorted set is not listpack-encoded");
    return;
  }
  const data_types::RangeByRankSpec spec(start, start + kRangeSize - 1, false,
                                         false, std::nullopt, reverse);
  for (auto _ : state) {
    (void)_;
    size_t visited = 0;
    zset->ForEachInRankRange(
        &spec, [&visited](std::string_view, double) { ++visited; });
    benchmark::DoNotOptimize(visited);
  }
  state.SetItemsProcessed(state.iterations() * kRangeSize);
}

// HSET of new fields into a listpack-encoded hash.
void HashListpackSet(benchmark::State& state) {
  const auto fields = Names("field:", static_cast<size_t>(state.range(0)));
  const auto values = Names("value:", fields.size());
  for (auto _ : state) {
    (void)_;
    auto hash = data_types::Hash::Create();
    for (size_t i = 0; i < fields.size(); ++i) {
      hash->Set(fields[i], values[i]);
    }
    benchmark::DoNotOptimize(hash->Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(IntsetInsertAscending)->Arg(16)->Arg(512);
BENCHMARK(IntsetInsertShuffled)->Arg(16)->Arg(512);
BENCHMARK(ListpackAppend)->Arg(16)->Arg(128)->Arg(700);
BENCHMARK(ListpackPrepend)->Arg(16)->Arg(128)->Arg(700);
BENCHMARK(ListpackDeleteFirst)->Arg(128)->Arg(700);
BENCHMARK(ZSetListpackInsert)->Arg(16)->Arg(128);
BENCHMARK(ZSetListpackRangeByRank)
    ->ArgNames({"start", "reverse", "integral"})
    ->Args({0, 0, 0})
    ->Args({59, 0, 0})
    ->Args({118, 0, 0})
    ->Args({0, 1, 0})
    ->Args({59, 1, 0})
    ->Args({118, 1, 0})
    ->Args({118, 0, 1})
    ->Args({118, 1, 1});
BENCHMARK(HashListpackSet)->Arg(16)->Arg(128)->Arg(512);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
