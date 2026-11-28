#include <benchmark/benchmark.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "containers/dict.h"

// Dict benchmarks over a fixed set of keys. Each benchmark builds its own
// dict, so results do not depend on which benchmarks ran before it.
namespace redis_simple {
namespace {
using StringDict = containers::Dict<std::string, std::string>;

constexpr size_t kKeyCount = 100'000;

std::vector<std::string> Keys(std::string_view prefix) {
  std::vector<std::string> keys;
  keys.reserve(kKeyCount);
  for (size_t i = 0; i < kKeyCount; ++i) {
    keys.push_back(std::string(prefix) + std::to_string(i));
  }
  return keys;
}

std::unique_ptr<StringDict> FilledDict(const std::vector<std::string>& keys) {
  auto dict = StringDict::Create();
  for (const std::string& key : keys) {
    dict->Insert(key, "value");
  }
  return dict;
}

// A fixed seed keeps runs comparable.
std::mt19937 SeededRng() {
  constexpr uint32_t kSeed = 42;
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  return std::mt19937(kSeed);
}

// Inserts kKeyCount new keys into an empty dict, growing it as it goes.
void DictInsert(benchmark::State& state) {
  const std::vector<std::string> keys = Keys("key:");
  for (auto _ : state) {
    (void)_;
    auto dict = FilledDict(keys);
    benchmark::DoNotOptimize(dict->Size());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kKeyCount));
}

// Looks up random keys. Argument 1 looks up keys that are present; 0 keys
// that are not.
void DictFind(benchmark::State& state) {
  const bool present = state.range(0) != 0;
  const std::vector<std::string> keys = Keys("key:");
  const std::vector<std::string> missing = Keys("missing:");
  const auto dict = FilledDict(keys);
  const std::vector<std::string>& probes = present ? keys : missing;
  std::mt19937 generator = SeededRng();
  std::uniform_int_distribution<size_t> index(0, kKeyCount - 1);
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(dict->FindValue(probes[index(generator)]));
  }
  state.SetItemsProcessed(state.iterations());
}

// Overwrites the value of a random existing key.
void DictUpdate(benchmark::State& state) {
  const std::vector<std::string> keys = Keys("key:");
  const auto dict = FilledDict(keys);
  std::mt19937 generator = SeededRng();
  std::uniform_int_distribution<size_t> index(0, kKeyCount - 1);
  for (auto _ : state) {
    (void)_;
    dict->Set(keys[index(generator)], "updated");
  }
  state.SetItemsProcessed(state.iterations());
}

// Deletes a random key and inserts it again, so the dict keeps its size.
void DictDeleteAndInsert(benchmark::State& state) {
  const std::vector<std::string> keys = Keys("key:");
  const auto dict = FilledDict(keys);
  std::mt19937 generator = SeededRng();
  std::uniform_int_distribution<size_t> index(0, kKeyCount - 1);
  for (auto _ : state) {
    (void)_;
    const std::string& key = keys[index(generator)];
    dict->Delete(key);
    dict->Insert(key, "value");
  }
  state.SetItemsProcessed(state.iterations());
}

// The operations a benchmark runs on a rehashing dict before it builds the
// next one. Each moves at most one of the table's 131,072 buckets, and more
// than half of those hold a key, so the rehash is still under way at the
// last of them.
constexpr size_t kRehashingOps = 32'768;

// A dict of `keys` whose table has just begun to rehash into one twice its
// size, as it does when the dict grows.
std::unique_ptr<StringDict> RehashingDict(
    const std::vector<std::string>& keys) {
  auto dict = StringDict::Create(kKeyCount);
  for (const std::string& key : keys) {
    dict->Insert(key, "value");
  }
  dict->Reserve(2 * std::bit_ceil(kKeyCount));
  return dict;
}

// `count` random positions in the keys.
std::vector<size_t> RandomIndexes(size_t count) {
  std::mt19937 generator = SeededRng();
  std::uniform_int_distribution<size_t> index(0, kKeyCount - 1);
  std::vector<size_t> indexes(count);
  for (size_t& position : indexes) {
    position = index(generator);
  }
  return indexes;
}

// DictFind on a dict that is rehashing, where each lookup also moves a
// bucket to the new table: kRehashingOps lookups an iteration. Building the
// dict is not timed.
void DictFindWhileRehashing(benchmark::State& state) {
  const bool present = state.range(0) != 0;
  const std::vector<std::string> keys = Keys("key:");
  const std::vector<std::string> missing = Keys("missing:");
  const std::vector<std::string>& probes = present ? keys : missing;
  const std::vector<size_t> indexes = RandomIndexes(kRehashingOps);
  std::unique_ptr<StringDict> dict;
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    dict = RehashingDict(keys);
    state.ResumeTiming();
    for (const size_t index : indexes) {
      benchmark::DoNotOptimize(dict->FindValue(probes[index]));
    }
  }
  if (dict != nullptr && !dict->IsRehashing()) {
    state.SkipWithError("the rehash ended before the lookups did");
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(kRehashingOps));
}

// DictDeleteAndInsert on a dict that is rehashing: kRehashingOps / 2 pairs an
// iteration.
void DictDeleteAndInsertWhileRehashing(benchmark::State& state) {
  const std::vector<std::string> keys = Keys("key:");
  const std::vector<size_t> indexes = RandomIndexes(kRehashingOps / 2);
  std::unique_ptr<StringDict> dict;
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    dict = RehashingDict(keys);
    state.ResumeTiming();
    for (const size_t index : indexes) {
      const std::string& key = keys[index];
      dict->Delete(key);
      dict->Insert(key, "value");
    }
  }
  if (dict != nullptr && !dict->IsRehashing()) {
    state.SkipWithError("the rehash ended before the updates did");
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(kRehashingOps / 2));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(DictInsert)->Unit(benchmark::kMillisecond);
BENCHMARK(DictFind)->Arg(1)->Arg(0)->ArgName("present");
BENCHMARK(DictUpdate);
BENCHMARK(DictDeleteAndInsert);
BENCHMARK(DictFindWhileRehashing)
    ->Arg(1)
    ->Arg(0)
    ->ArgName("present")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(DictDeleteAndInsertWhileRehashing)->Unit(benchmark::kMicrosecond);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK_MAIN();
