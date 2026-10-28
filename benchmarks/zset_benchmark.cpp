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
// Fixed seeds keep runs comparable, which is the point of these benchmarks.
std::mt19937 SeededRng(uint32_t offset) {
  constexpr uint32_t kSeed = 42;
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  return std::mt19937(kSeed + offset);
}

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
  auto rng = SeededRng(0);
  std::uniform_real_distribution<double> score(0, 1'000'000);
  for (const auto& name : names) {
    zset->InsertOrUpdate(name, score(rng));
  }
  return zset;
}

// ZADD of new members into an initially empty set.
void ZSetInsert(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    (void)_;
    auto zset = FilledZSet(names);
    benchmark::DoNotOptimize(zset->Size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// ZADD changing the score of an existing member.
void ZSetUpdateScore(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  auto rng = SeededRng(1);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  std::uniform_real_distribution<double> score(0, 1'000'000);
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(
        zset->InsertOrUpdate(names[member(rng)], score(rng)));
  }
  state.SetItemsProcessed(state.iterations());
}

// ZREM then re-ZADD of a random member.
void ZSetDeleteRandom(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  auto rng = SeededRng(2);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  for (auto _ : state) {
    (void)_;
    const std::string& name = names[member(rng)];
    const double score = zset->Score(name).value_or(0.0);
    zset->Delete(name);
    zset->InsertOrUpdate(name, score);
  }
  state.SetItemsProcessed(state.iterations());
}

// ZREM then re-ZADD of the alphabetically smallest member.
void ZSetDeleteSmallestName(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  const std::string& name = names.front();
  const double score = zset->Score(name).value_or(0.0);
  for (auto _ : state) {
    (void)_;
    zset->Delete(name);
    zset->InsertOrUpdate(name, score);
  }
  state.SetItemsProcessed(state.iterations());
}

// ZRANK of a random member.
void ZSetRank(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  auto rng = SeededRng(3);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(zset->Rank(names[member(rng)]));
  }
  state.SetItemsProcessed(state.iterations());
}

// ZRANGEBYSCORE over a random window holding about 100 members, with an
// exclusive lower bound.
void ZSetRangeByScore(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const auto names = MemberNames(size);
  auto zset = FilledZSet(names);
  const double width = 1'000'000.0 * 100 / static_cast<double>(size);
  auto rng = SeededRng(4);
  std::uniform_real_distribution<double> start(0, 1'000'000 - width);
  for (auto _ : state) {
    (void)_;
    const double min = start(rng);
    const zset::RangeByScoreSpec spec(min, min + width, true, false);
    benchmark::DoNotOptimize(zset->RangeByScore(&spec));
  }
  state.SetItemsProcessed(state.iterations());
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ZSetInsert)->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMillisecond);
BENCHMARK(ZSetUpdateScore)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetDeleteRandom)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetDeleteSmallestName)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetRank)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetRangeByScore)->Arg(10'000)->Arg(100'000);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
