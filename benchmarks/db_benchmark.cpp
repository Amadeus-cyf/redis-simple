#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "utils/time.h"

// Keyspace benchmarks: lookups of keys with and without a TTL, string SETs
// by value size, as new keys and as overwrites, and the SET command's lookup
// followed by its store.
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

// GET-style lookups. Argument 1 gives one key a TTL and 2 gives every key
// one, so each lookup compares the key's expiration time with the clock.
void DbLookup(benchmark::State& state) {
  const std::vector<std::string> keys = Keys();
  auto db = db::RedisDb::Create();
  for (const std::string& key : keys) {
    db->SetKey(key, db::RedisObject::CreateWithString("value"), 0);
  }
  const std::array<size_t, 3> ttl_counts = {0, 1, keys.size()};
  const size_t keys_with_ttl = ttl_counts[static_cast<size_t>(state.range(0))];
  for (size_t i = 0; i < keys_with_ttl; ++i) {
    db->ExpireKeyAt(keys[i], utils::NowInMilliseconds() + 3'600'000);
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
  const std::vector<std::string> keys = Keys();
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

// SET of string values by size: 3 bytes fits std::string's inline buffer,
// 36 fits an embedded string, and 100 needs its own allocation. Argument
// `overwrite` 0 inserts new keys, which allocate an object (and, for long
// values, a buffer) each; 1 overwrites existing keys in place.
void DbSetString(benchmark::State& state) {
  const auto value_size = static_cast<size_t>(state.range(0));
  const bool overwrite = state.range(1) != 0;
  const std::string value(value_size, 'v');
  const std::vector<std::string> keys = Keys();
  auto db = db::RedisDb::Create();
  if (overwrite) {
    for (const std::string& key : keys) {
      db->SetStringKey(key, value, 0);
    }
  }
  size_t next = 0;
  for (auto _ : state) {
    (void)_;
    db->SetStringKey(keys[next], value, 0);
    next = (next + 1) % keys.size();
    if (next == 0 && !overwrite) {
      // Start the next pass from an empty keyspace so every SET inserts.
      state.PauseTiming();
      db->Flush();
      state.ResumeTiming();
    }
  }
  state.SetItemsProcessed(state.iterations());
}

// What the SET command does to the keyspace: look the key up, then store the
// value through the slot the lookup found. Argument `overwrite` 0 sets new
// keys and 1 existing ones; `with_ttl` 1 gives one other key a TTL, so the
// database has a TTL index while the SETs run.
void DbSetCommand(benchmark::State& state) {
  const bool overwrite = state.range(0) != 0;
  const bool with_ttl = state.range(1) != 0;
  const std::vector<std::string> keys = Keys();
  auto db = db::RedisDb::Create();
  const auto fill = [&] {
    if (overwrite) {
      for (const std::string& key : keys) {
        db->SetStringKey(key, "initial-value", 0);
      }
    }
    if (with_ttl) {
      db->SetStringKey("volatile", "value",
                       utils::NowInMilliseconds() + 3'600'000);
    }
  };
  fill();
  size_t next = 0;
  for (auto _ : state) {
    (void)_;
    db->SetStringAt(db->FindOrCreateKey(keys[next]), "updated-value", 0);
    next = (next + 1) % keys.size();
    if (next == 0 && !overwrite) {
      // Start the next pass from an empty keyspace so every SET inserts.
      state.PauseTiming();
      db->Flush();
      fill();
      state.ResumeTiming();
    }
  }
  state.SetItemsProcessed(state.iterations());
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(DbLookup)->Arg(0)->Arg(1)->Arg(2)->ArgName("with_ttl");
BENCHMARK(DbOverwriteString)->Arg(0)->Arg(1)->ArgName("in_place");
BENCHMARK(DbSetString)
    ->ArgsProduct({{3, 36, 100}, {0, 1}})
    ->ArgNames({"value_bytes", "overwrite"});
BENCHMARK(DbSetCommand)
    ->ArgsProduct({{0, 1}, {0, 1}})
    ->ArgNames({"overwrite", "with_ttl"});
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
