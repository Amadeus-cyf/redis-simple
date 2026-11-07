#include <benchmark/benchmark.h>

#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "memory/dict.h"

namespace redis_simple {
in_memory::Dict<std::string, std::string>& BenchmarkDict() {
  // Leaked, so the static has no destructor to run at exit.
  static auto& dict =
      *in_memory::Dict<std::string, std::string>::Create().release();
  return dict;
}

std::vector<std::string>& BenchmarkKeys() {
  static auto& keys = *new std::vector<std::string>;
  return keys;
}

static std::mt19937& Rng() {
  static std::mt19937 rng(std::random_device{}());
  return rng;
}

static size_t RandomIndex(size_t size) {
  std::uniform_int_distribution<size_t> distribution(0, size - 1);
  return distribution(Rng());
}

static std::string RandomString(int length) {
  static constexpr std::string_view kAlphaNum =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  std::uniform_int_distribution<size_t> distribution(0, kAlphaNum.size() - 1);
  std::string value;
  value.reserve(length);

  for (int i = 0; i < length; ++i) {
    value += kAlphaNum[distribution(Rng())];
  }

  return value;
}

static void DictAdd(benchmark::State& state) {
  for (auto _ : state) {
    (void)_;
    const std::string key = RandomString(10);
    BenchmarkKeys().push_back(key);
    BenchmarkDict().Insert(key, RandomString(10));
  }
}

static void DictFind(benchmark::State& state) {
  for (auto _ : state) {
    (void)_;
    if (!BenchmarkKeys().empty()) {
      benchmark::DoNotOptimize(BenchmarkDict().Get(
          BenchmarkKeys()[RandomIndex(BenchmarkKeys().size())]));
    }
  }
}

static void DictUpdate(benchmark::State& state) {
  std::bernoulli_distribution use_existing(0.5);
  for (auto _ : state) {
    (void)_;
    if (use_existing(Rng()) && !BenchmarkKeys().empty()) {
      BenchmarkDict().Set(BenchmarkKeys()[RandomIndex(BenchmarkKeys().size())],
                          RandomString(10));
    } else {
      BenchmarkDict().Set("non-existing key", "val");
    }
  }
}

static void DictDelete(benchmark::State& state) {
  std::bernoulli_distribution use_existing(0.5);
  for (auto _ : state) {
    (void)_;
    if (use_existing(Rng()) && !BenchmarkKeys().empty()) {
      BenchmarkDict().Delete(
          BenchmarkKeys()[RandomIndex(BenchmarkKeys().size())]);
    } else {
      BenchmarkDict().Delete("non-existing key");
    }
  }
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(DictAdd);
BENCHMARK(DictFind);
BENCHMARK(DictUpdate);
BENCHMARK(DictDelete);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK_MAIN();
