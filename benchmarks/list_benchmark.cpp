#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "data_types/list/list.h"

// Index and range benchmarks are sized well above the listpack limit, so the
// list uses the quicklist encoding with hundreds of nodes.
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

// LREM count 0 on a list where every tenth element matches, so each call
// scans every element and deletes a tenth of them. The list is rebuilt
// outside the timed region.
void ListRemove(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    auto list = list::List::Create();
    for (size_t i = 0; i < size; ++i) {
      list->RPush(i % 10 == 0 ? "match" : "item:" + std::to_string(i));
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(
        list->Remove("match", 0, list::List::RemoveDirection::kFromHead));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(size));
}

// LREM of a value the list does not hold: a pure scan.
void ListRemoveMissing(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const auto list = FilledList(size);
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(
        list->Remove("missing", 0, list::List::RemoveDirection::kFromHead));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(size));
}

// The push that grows a full listpack past its size limit and converts the
// list to a quicklist.
void ListConvertToQuickList(benchmark::State& state) {
  const auto item = [](size_t i) { return "item:" + std::to_string(i); };
  // The most elements a listpack-encoded list holds before converting.
  size_t full_size = 0;
  for (auto probe = list::List::Create();
       probe->Encoding() == list::List::Encoding::kListPack; ++full_size) {
    probe->RPush(item(full_size));
  }
  --full_size;
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    auto list = list::List::Create();
    for (size_t i = 0; i < full_size; ++i) {
      list->RPush(item(i));
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(list->RPush(item(full_size)));
  }
  state.counters["elements"] = static_cast<double>(full_size);
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(ListIndex)
    ->ArgsProduct({{300'000}, {0, 50, 100}})
    ->ArgNames({"size", "percent"});
BENCHMARK(ListRange)
    ->ArgsProduct({{300'000}, {0, 50, 100}})
    ->ArgNames({"size", "percent"});
// 100 elements fit in one listpack; 10,000 use a quicklist.
BENCHMARK(ListRemove)->Arg(100)->Arg(10'000)->ArgName("size");
BENCHMARK(ListRemoveMissing)->Arg(100)->Arg(10'000)->ArgName("size");
BENCHMARK(ListConvertToQuickList);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
