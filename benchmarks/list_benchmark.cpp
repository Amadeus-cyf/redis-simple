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
// The list item at `index`: "item:<index>" followed by 0 to 7 'x' characters,
// the count picked by a hash of the index, so that neighbouring items differ
// in length. With items of one length every entry in a quicklist node has the
// same size, and a CPU that predicts the values it loads (Apple silicon does)
// then guesses each entry's size instead of waiting for it. How much that
// helped changed by 10 to 20% from one build or launch to the next on an M5,
// which is more than most changes these benchmarks are run to measure.
std::string Item(size_t index) {
  // The SplitMix64 finalizer, so that one index's hash says nothing about the
  // next one's.
  uint64_t hash = index;
  hash = (hash ^ (hash >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  hash = (hash ^ (hash >> 27U)) * 0x94d049bb133111ebULL;
  hash ^= hash >> 31U;
  constexpr uint64_t kPaddingLengths = 8;
  std::string item = "item:" + std::to_string(index);
  item.append(static_cast<size_t>(hash % kPaddingLengths), 'x');
  return item;
}

std::unique_ptr<data_types::List> FilledList(size_t size) {
  auto list = data_types::List::Create();
  for (size_t i = 0; i < size; ++i) {
    list->RPush(Item(i));
  }
  return list;
}

// The position of a benchmark's `call`th read, for an argument that is a
// percentage of the list: 0 is the head, 50 the middle, and 100 the tail. The
// head and the tail are one position each. A read in between also walks its
// quicklist node from the node's closer end, which takes no steps for the
// node's first element and the most for its middle one, so one position would
// measure where a node happens to start. Those reads step through a window of
// about two nodes instead.
size_t Position(size_t size, int64_t percent, size_t call) {
  const size_t position = (size - 1) * static_cast<size_t>(percent) / 100;
  if (percent == 0 || percent == 100) {
    return position;
  }
  // The stride is odd, so the calls reach every position in the window, and
  // long, so that one call's walk says little about the next one's.
  constexpr size_t kWindow = 1024;
  constexpr size_t kStride = 331;
  return std::min(position + ((call * kStride) % kWindow), size - 1);
}

// LINDEX at the head, middle, or tail. Seeks walk from the closer end, so the
// head and tail should cost the same and the middle the most.
void ListIndex(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const auto list = FilledList(size);
  const int64_t percent = state.range(1);
  size_t call = 0;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(list->At(Position(size, percent, call++)));
  }
  state.SetItemsProcessed(state.iterations());
}

// LRANGE of 100 elements starting at the head, middle, or tail.
void ListRange(benchmark::State& state) {
  constexpr size_t kCount = 100;
  const auto size = static_cast<size_t>(state.range(0));
  const auto list = FilledList(size);
  const int64_t percent = state.range(1);
  size_t call = 0;
  for (auto _ : state) {
    (void)_;
    const size_t start =
        std::min(Position(size, percent, call++), size - kCount);
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
    auto list = data_types::List::Create();
    for (size_t i = 0; i < size; ++i) {
      list->RPush(i % 10 == 0 ? "match" : Item(i));
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(
        list->Remove("match", 0, data_types::List::RemoveDirection::kFromHead));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(size));
}

// LREM of a value the list does not hold: a pure scan.
void ListRemoveMissing(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const auto list = FilledList(size);
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(list->Remove(
        "missing", 0, data_types::List::RemoveDirection::kFromHead));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(size));
}

// The push that grows a full listpack past its size limit and converts the
// list to a quicklist.
void ListConvertToQuicklist(benchmark::State& state) {
  // The most elements a listpack-encoded list holds before converting.
  size_t full_size = 0;
  for (auto probe = data_types::List::Create();
       probe->Encoding() == data_types::List::Encoding::kListpack;
       ++full_size) {
    probe->RPush(Item(full_size));
  }
  --full_size;
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    auto list = data_types::List::Create();
    for (size_t i = 0; i < full_size; ++i) {
      list->RPush(Item(i));
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(list->RPush(Item(full_size)));
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
BENCHMARK(ListConvertToQuicklist);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
