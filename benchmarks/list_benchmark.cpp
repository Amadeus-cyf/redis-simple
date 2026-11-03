#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "data_types/list/list.h"

// List benchmarks sized well above the listpack limit, so every list uses the
// quicklist encoding with hundreds of nodes.
namespace redis_simple {
namespace {
std::unique_ptr<list::List> FilledList(size_t size) {
  auto list = list::List::Create();
  for (size_t i = 0; i < size; ++i) {
    list->RPush("item:" + std::to_string(i));
  }
  return list;
}

// Element position for a benchmark argument: 0 is the head, 50 the middle,
// and 100 the tail, as a percentage of the list.
size_t Position(size_t size, int64_t percent) {
  return (size - 1) * static_cast<size_t>(percent) / 100;
}

// LINDEX at the head, middle, or tail. Seeks walk from the closer end, so the
// head and tail should cost the same and the middle the most.
void ListIndex(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const auto list = FilledList(size);
  const size_t position = Position(size, state.range(1));
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(list->At(position));
  }
  state.SetItemsProcessed(state.iterations());
}

// LRANGE of 100 elements starting at the head, middle, or tail.
void ListRange(benchmark::State& state) {
  constexpr size_t kCount = 100;
  const auto size = static_cast<size_t>(state.range(0));
  const auto list = FilledList(size);
  const size_t start = std::min(Position(size, state.range(1)), size - kCount);
  for (auto _ : state) {
    (void)_;
    size_t bytes = 0;
    list->ForEach(start, start + kCount - 1, [&bytes](std::string_view value) {
      bytes += value.size();
      return true;
    });
    benchmark::DoNotOptimize(bytes);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kCount));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ListIndex)
    ->ArgsProduct({{300'000}, {0, 50, 100}})
    ->ArgNames({"size", "percent"});
BENCHMARK(ListRange)
    ->ArgsProduct({{300'000}, {0, 50, 100}})
    ->ArgNames({"size", "percent"});
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
