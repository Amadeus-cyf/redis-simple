#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "server/db/db.h"
#include "server/db/redis_obj.h"
#include "utils/time_utils.h"

// Keyspace benchmarks: lookups that also consult the TTL table, and string
// overwrites as SET does them.
namespace redis_simple {
namespace {
constexpr size_t kKeyCount = 100'000;

std::vector<std::string> Keys() {
  std::vector<std::string> keys;
  keys.reserve(kKeyCount);
  for (size_t i = 0; i < kKeyCount; ++i) {
    keys.push_back("key:" + std::to_string(i));
  }
  return keys;
}

// GET-style lookups. Argument 1 gives one key a TTL, so every lookup also
// checks the TTL table, which reuses the key's hash.
void DbLookup(benchmark::State& state) {
  const auto keys = Keys();
  auto db = db::RedisDb::Create();
  for (const std::string& key : keys) {
    db->SetKey(key, db::RedisObject::CreateWithString("value"), 0);
  }
  if (state.range(0) != 0) {
    db->ExpireKeyAt(keys.front(), utils::NowInMilliseconds() + 3'600'000);
  }
  size_t next = 0;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(db->LookupKey(keys[next]));
    next = (next + 1) % keys.size();
  }
  state.SetItemsProcessed(state.iterations());
}

// SET overwriting existing string keys. Argument 1 updates the string object
// in place through SetStringKey; 0 allocates a new object, as SetKey does.
void DbOverwriteString(benchmark::State& state) {
  const bool in_place = state.range(0) != 0;
  const auto keys = Keys();
  auto db = db::RedisDb::Create();
  for (const std::string& key : keys) {
    db->SetStringKey(key, "initial-value", 0);
  }
  size_t next = 0;
  for (auto _ : state) {
    (void)_;
    const std::string& key = keys[next];
    if (in_place) {
      db->SetStringKey(key, "updated-value", 0);
    } else {
      db->SetKey(key, db::RedisObject::CreateWithString("updated-value"), 0);
    }
    next = (next + 1) % keys.size();
  }
  state.SetItemsProcessed(state.iterations());
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(DbLookup)->Arg(0)->Arg(1)->ArgName("with_ttl");
BENCHMARK(DbOverwriteString)->Arg(0)->Arg(1)->ArgName("in_place");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
