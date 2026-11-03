#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "data_types/set/set.h"
#include "memory/dict.h"

// Set-intersection benchmarks over dict-encoded sets, comparing probes that
// reuse each member's stored hash with probes that rehash it.
namespace redis_simple {
namespace {
// Members 0, step, 2 * step, ..., so sets built with steps 1 and 2 share
// every second member of the first.
std::unique_ptr<set::Set> FilledSet(size_t count, size_t step) {
  auto set = set::Set::Create();
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
        [&](std::string_view member, const in_memory::HashedKey* hashed) {
          matches +=
              right->HasMember(member, reuse_hash ? hashed : nullptr) ? 1 : 0;
          return true;
        });
    benchmark::DoNotOptimize(matches);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(SetIntersect)
    ->ArgsProduct({{2'000, 100'000}, {0, 1}})
    ->ArgNames({"size", "reuse_hash"});
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
