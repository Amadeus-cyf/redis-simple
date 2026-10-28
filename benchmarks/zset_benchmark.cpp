#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "data_types/zset/zset.h"
#include "data_types/zset/zset_range_spec.h"

// Sorted-set benchmarks sized well above the listpack limit, so every set uses
// the skiplist encoding. Random sequences use fixed seeds so runs compare.
namespace redis_simple {
namespace {
constexpr uint32_t kSeed = 42;

std::vector<std::string> MemberNames(size_t count) {
  std::vector<std::string> names;
  names.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    // Zero-padded so name order matches index order; "member:000000" is the
    // alphabetically smallest member.
    std::string name = std::to_string(i);
    names.push_back("member:" + std::string(6 - name.size(), '0') + name);
  }
  return names;
}

std::unique_ptr<zset::ZSet> FilledZSet(const std::vector<std::string>& names) {
  auto zset = zset::ZSet::Create();
  std::mt19937 rng(kSeed);
  std::uniform_real_distribution<double> score(0, 1'000'000);
  for (const auto& name : names) {
    zset->InsertOrUpdate(name, score(rng));
  }
  return zset;
}

// ZADD of new members into an initially empty set.
void BM_ZSetInsert(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    auto zset = FilledZSet(names);
    benchmark::DoNotOptimize(zset->Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ZSetInsert)
    ->Arg(10'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond);

// ZADD changing the score of an existing member.
void BM_ZSetUpdateScore(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  std::mt19937 rng(kSeed + 1);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  std::uniform_real_distribution<double> score(0, 1'000'000);
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        zset->InsertOrUpdate(names[member(rng)], score(rng)));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ZSetUpdateScore)->Arg(10'000)->Arg(100'000);

// ZREM then re-ZADD of a random member.
void BM_ZSetDeleteRandom(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  std::mt19937 rng(kSeed + 2);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  for (auto _ : state) {
    const std::string& name = names[member(rng)];
    const double score = *zset->Score(name);
    zset->Delete(name);
    zset->InsertOrUpdate(name, score);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ZSetDeleteRandom)->Arg(10'000)->Arg(100'000);

// ZREM then re-ZADD of the alphabetically smallest member.
void BM_ZSetDeleteSmallestName(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  const std::string& name = names.front();
  const double score = *zset->Score(name);
  for (auto _ : state) {
    zset->Delete(name);
    zset->InsertOrUpdate(name, score);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ZSetDeleteSmallestName)->Arg(10'000)->Arg(100'000);

// ZRANK of a random member.
void BM_ZSetRank(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  std::mt19937 rng(kSeed + 3);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  for (auto _ : state) {
    benchmark::DoNotOptimize(zset->Rank(names[member(rng)]));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ZSetRank)->Arg(10'000)->Arg(100'000);

// ZRANGEBYSCORE over a random window holding about 100 members, with an
// exclusive lower bound.
void BM_ZSetRangeByScore(benchmark::State& state) {
  const size_t size = static_cast<size_t>(state.range(0));
  const auto names = MemberNames(size);
  auto zset = FilledZSet(names);
  const double width = 1'000'000.0 * 100 / static_cast<double>(size);
  std::mt19937 rng(kSeed + 4);
  std::uniform_real_distribution<double> start(0, 1'000'000 - width);
  for (auto _ : state) {
    const double min = start(rng);
    const zset::RangeByScoreSpec spec(min, min + width, true, false);
    benchmark::DoNotOptimize(zset->RangeByScore(&spec));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ZSetRangeByScore)->Arg(10'000)->Arg(100'000);
}  // namespace
}  // namespace redis_simple
