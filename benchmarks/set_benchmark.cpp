#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "containers/dict.h"
#include "data_types/set/set.h"
#include "data_types/set/set_algebra.h"

// Set-intersection benchmarks over dict-encoded sets, comparing probes that
// reuse each member's stored hash with probes that rehash it, and benchmarks
// of SINTER and SDIFF on sets of very different sizes.
namespace redis_simple {
namespace {
// Members 0, step, 2 * step, ..., so sets built with steps 1 and 2 share
// every second member of the first.
std::unique_ptr<data_types::Set> FilledSet(size_t count, size_t step) {
  auto set = data_types::Set::Create();
  for (size_t i = 0; i < count; ++i) {
    set->Add("member:" + std::to_string(i * step));
  }
  return set;
}

// The SINTER inner loop: walk one set and test membership in another.
// Argument 1 reuses the stored hash, as SINTER does; 0 rehashes each member.
void SetIntersect(benchmark::State& state) {
  const auto size = static_cast<size_t>(state.range(0));
  const bool reuse_hash = state.range(1) != 0;
  const auto left = FilledSet(size, 1);
  const auto right = FilledSet(size, 2);
  for (auto _ : state) {
    (void)_;
    size_t matches = 0;
    left->ForEachMemberWithHash(
        [&](std::string_view member, const containers::HashedKey* hashed) {
          matches +=
              right->HasMember(member, reuse_hash ? hashed : nullptr) ? 1 : 0;
          return true;
        });
    benchmark::DoNotOptimize(matches);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Members first, first + 1, ..., as "member:<number>".
std::unique_ptr<data_types::Set> RangeSet(size_t first, size_t count) {
  auto set = data_types::Set::Create();
  for (size_t i = 0; i < count; ++i) {
    set->Add("member:" + std::to_string(first + i));
  }
  return set;
}

// SINTER of a walked set of 10,000 members, a set of 100,000 that holds all
// of them and a set of 20,000 that holds none, so the result is empty.
// Argument 0 names the large set before the one that rules every member
// out, 1 after it.
void SetIntersectThree(benchmark::State& state) {
  constexpr size_t kWalked = 10'000;
  const auto walked = RangeSet(0, kWalked);
  const auto superset = RangeSet(0, 100'000);
  const auto disjoint = RangeSet(200'000, 20'000);
  const bool disjoint_first = state.range(0) != 0;
  for (auto _ : state) {
    (void)_;
    std::vector<const data_types::Set*> sets{
        walked.get(), disjoint_first ? disjoint.get() : superset.get(),
        disjoint_first ? superset.get() : disjoint.get()};
    size_t matches = 0;
    data_types::ForEachCommonMember(
        std::span(sets), [&matches](std::string_view) { ++matches; });
    benchmark::DoNotOptimize(matches);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kWalked));
}

// SDIFF of a set of 100,000 members and `others` sets of `other_size`
// members each, all of them members of the first set. Argument `algorithm`:
// 0 probes, 1 subtracts, 2 lets SDIFF's estimate choose.
void SetDifference(benchmark::State& state) {
  constexpr size_t kFirstSize = 100'000;
  const auto other_count = static_cast<size_t>(state.range(0));
  const auto other_size = static_cast<size_t>(state.range(1));
  const auto first = RangeSet(0, kFirstSize);
  std::vector<std::unique_ptr<data_types::Set>> others;
  others.reserve(other_count);
  for (size_t i = 0; i < other_count; ++i) {
    others.push_back(RangeSet(i * other_size, other_size));
  }
  for (auto _ : state) {
    (void)_;
    std::vector<const data_types::Set*> sets;
    sets.reserve(others.size());
    for (const auto& set : others) {
      sets.push_back(set.get());
    }
    size_t remaining = 0;
    const auto count = [&remaining](std::string_view) { ++remaining; };
    switch (state.range(2)) {
      case 0:
        data_types::ForEachDifferenceMemberByProbing(*first, std::span(sets),
                                                     count);
        break;
      case 1:
        data_types::ForEachDifferenceMemberBySubtracting(
            *first, std::span(sets), count);
        break;
      default:
        data_types::ForEachDifferenceMember(*first, std::span(sets), count);
        break;
    }
    benchmark::DoNotOptimize(remaining);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(kFirstSize));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(SetIntersect)
    ->ArgsProduct({{2'000, 100'000}, {0, 1}})
    ->ArgNames({"size", "reuse_hash"});
BENCHMARK(SetIntersectThree)->Arg(0)->Arg(1)->ArgName("disjoint_first");
BENCHMARK(SetDifference)
    ->ArgsProduct({{2, 4, 8, 32}, {10, 1'000, 100'000}, {0, 1, 2}})
    ->ArgNames({"others", "other_size", "algorithm"});
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
