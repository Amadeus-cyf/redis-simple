#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "containers/intset.h"
#include "containers/listpack.h"
#include "data_types/hash/hash.h"
#include "data_types/zset/zset.h"

// Inserts into the compact encodings (intset and listpack), where each insert
// may reallocate the whole value and moves the bytes after it. Sizes stay
// within the encodings' limits, except the listpack ones marked otherwise.
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

// RPUSH: every element goes after the last one. The larger size is a full
// 8 KiB quicklist node.
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
BENCHMARK(HashListpackSet)->Arg(16)->Arg(128)->Arg(512);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
