#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "data_types/zset/zset.h"
#include "data_types/zset/zset_range_spec.h"
#include "utils/floats.h"

// Sorted-set benchmarks. Sizes of 10,000 and more are well above the listpack
// limit, so those sets use the skiplist encoding; a size of 100 stays in a
// listpack. Random sequences use fixed seeds so runs compare.
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

std::unique_ptr<data_types::ZSet> FilledZSet(
    const std::vector<std::string>& names) {
  auto zset = data_types::ZSet::Create();
  std::mt19937 generator = SeededRng(0);
  std::uniform_real_distribution<double> score(0, 1'000'000);
  for (const auto& name : names) {
    zset->InsertOrUpdate(name, score(generator));
  }
  return zset;
}

// A set whose members share one score, which is what the lexicographic
// commands are defined for.
std::unique_ptr<data_types::ZSet> FilledZSetWithOneScore(
    const std::vector<std::string>& names) {
  auto zset = data_types::ZSet::Create();
  for (const auto& name : names) {
    zset->InsertOrUpdate(name, 0);
  }
  return zset;
}

data_types::LexBound LexValue(std::string_view member) {
  return {.kind = data_types::LexBound::Kind::kValue, .value = member};
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
  std::mt19937 generator = SeededRng(1);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  std::uniform_real_distribution<double> score(0, 1'000'000);
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(
        zset->InsertOrUpdate(names[member(generator)], score(generator)));
  }
  state.SetItemsProcessed(state.iterations());
}

// ZREM then re-ZADD of a random member.
void ZSetDeleteRandom(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  std::mt19937 generator = SeededRng(2);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  for (auto _ : state) {
    (void)_;
    const std::string& name = names[member(generator)];
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

// ZADD GT of a random existing member with a score above every other one, so
// each call updates the member.
void ZSetAddGreaterThan(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  std::mt19937 generator = SeededRng(5);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  double score = 1'000'000;
  for (auto _ : state) {
    (void)_;
    score += 1;
    benchmark::DoNotOptimize(
        zset->Add(names[member(generator)], score, {.gt = true}));
  }
  state.SetItemsProcessed(state.iterations());
}

// Parsing the bounds of a score range, as ZRANGEBYSCORE and ZCOUNT do.
void ZSetParseScoreBounds(benchmark::State& state) {
  constexpr std::array<std::string_view, 4> kBounds = {"-inf", "+inf", "1.5",
                                                       "1234567.25"};
  for (auto _ : state) {
    (void)_;
    for (std::string_view bound : kBounds) {
      benchmark::DoNotOptimize(bound);
      benchmark::DoNotOptimize(utils::ToRangeDouble(bound));
    }
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(kBounds.size()));
}

// ZRANK of a random member.
void ZSetRank(benchmark::State& state) {
  const auto names = MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSet(names);
  std::mt19937 generator = SeededRng(3);
  std::uniform_int_distribution<size_t> member(0, names.size() - 1);
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(zset->Rank(names[member(generator)]));
  }
  state.SetItemsProcessed(state.iterations());
}

// ZRANGEBYSCORE over a random window holding about 100 members, with an
// exclusive lower bound.
void ZSetRangeByScore(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const std::vector<std::string> names = MemberNames(size);
  auto zset = FilledZSet(names);
  const double width = 1'000'000.0 * 100 / static_cast<double>(size);
  std::mt19937 generator = SeededRng(4);
  std::uniform_real_distribution<double> start(0, 1'000'000 - width);
  for (auto _ : state) {
    (void)_;
    const double min = start(generator);
    const data_types::RangeByScoreSpec spec(min, min + width, true, false);
    // Visit the range as ZRANGEBYSCORE does, without collecting it.
    size_t visited = 0;
    zset->ForEachInScoreRange(
        &spec, [&visited](std::string_view, double) { ++visited; });
    benchmark::DoNotOptimize(visited);
  }
  state.SetItemsProcessed(state.iterations());
}

// ZRANGEBYSCORE min +inf LIMIT offset 10 from a random min, or, when the
// third argument is 1, ZREVRANGEBYSCORE max -inf LIMIT offset 10 from a
// random max. The bound leaves at least half the set in the range, so the
// offset is always inside it.
void ZSetRangeByScoreWithOffset(benchmark::State& state) {
  const std::vector<std::string> names =
      MemberNames(static_cast<size_t>(state.range(0)));
  const data_types::LimitSpec limit(static_cast<size_t>(state.range(1)), 10);
  const bool reverse = state.range(2) != 0;
  auto zset = FilledZSet(names);
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  std::mt19937 generator = SeededRng(6);
  std::uniform_real_distribution<double> edge(0, 500'000);
  for (auto _ : state) {
    (void)_;
    const double bound = edge(generator);
    const data_types::RangeByScoreSpec spec =
        reverse ? data_types::RangeByScoreSpec(-kInfinity, 1'000'000 - bound,
                                               false, false, limit, true)
                : data_types::RangeByScoreSpec(bound, kInfinity, false, false,
                                               limit);
    size_t visited = 0;
    zset->ForEachInScoreRange(
        &spec, [&visited](std::string_view, double) { ++visited; });
    benchmark::DoNotOptimize(visited);
  }
  state.SetItemsProcessed(state.iterations());
}

// ZCOUNT of a random window holding about 100 members.
void ZSetCount(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const std::vector<std::string> names = MemberNames(size);
  auto zset = FilledZSet(names);
  const double width = 1'000'000.0 * 100 / static_cast<double>(size);
  std::mt19937 generator = SeededRng(7);
  std::uniform_real_distribution<double> start(0, 1'000'000 - width);
  for (auto _ : state) {
    (void)_;
    const double min = start(generator);
    const data_types::RangeByScoreSpec spec(min, min + width, false, false);
    benchmark::DoNotOptimize(zset->Count(&spec));
  }
  state.SetItemsProcessed(state.iterations());
}

// ZRANGEBYLEX of 100 members from a random one, in a set with one score.
void ZSetRangeByLex(benchmark::State& state) {
  constexpr size_t kWindow = 100;
  const std::vector<std::string> names =
      MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSetWithOneScore(names);
  std::mt19937 generator = SeededRng(8);
  std::uniform_int_distribution<size_t> start(0, names.size() - kWindow);
  for (auto _ : state) {
    (void)_;
    const size_t first = start(generator);
    const data_types::RangeByLexSpec spec{
        .min = LexValue(names[first]),
        .max = LexValue(names[first + kWindow - 1]),
        .minex = false,
        .maxex = false,
        .limit = std::nullopt,
        .reverse = false};
    size_t visited = 0;
    zset->ForEachInLexRange(
        &spec, [&visited](std::string_view, double) { ++visited; });
    benchmark::DoNotOptimize(visited);
  }
  state.SetItemsProcessed(state.iterations());
}

// ZLEXCOUNT of 100 members from a random one, in a set with one score.
void ZSetLexCount(benchmark::State& state) {
  constexpr size_t kWindow = 100;
  const std::vector<std::string> names =
      MemberNames(static_cast<size_t>(state.range(0)));
  auto zset = FilledZSetWithOneScore(names);
  std::mt19937 generator = SeededRng(9);
  std::uniform_int_distribution<size_t> start(0, names.size() - kWindow);
  for (auto _ : state) {
    (void)_;
    const size_t first = start(generator);
    const data_types::RangeByLexSpec spec{
        .min = LexValue(names[first]),
        .max = LexValue(names[first + kWindow - 1]),
        .minex = false,
        .maxex = false,
        .limit = std::nullopt,
        .reverse = false};
    benchmark::DoNotOptimize(zset->LexCount(&spec));
  }
  state.SetItemsProcessed(state.iterations());
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ZSetInsert)->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMillisecond);
BENCHMARK(ZSetUpdateScore)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetDeleteRandom)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetDeleteSmallestName)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetAddGreaterThan)->Arg(100)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetParseScoreBounds);
BENCHMARK(ZSetRank)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetRangeByScore)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetRangeByScoreWithOffset)
    ->Args({100'000, 5, 0})
    ->Args({100'000, 100, 0})
    ->Args({100'000, 10'000, 0})
    ->Args({100'000, 5, 1})
    ->Args({100'000, 100, 1})
    ->Args({100'000, 10'000, 1});
BENCHMARK(ZSetCount)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetRangeByLex)->Arg(10'000)->Arg(100'000);
BENCHMARK(ZSetLexCount)->Arg(10'000)->Arg(100'000);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
