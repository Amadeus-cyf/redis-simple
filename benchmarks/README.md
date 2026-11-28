# Benchmarks

Benchmarks use Google Benchmark and build into one `redis_simple_benchmarks`
executable. Build it with the release preset, since debug timings are not
representative:

```sh
cmake --preset release
cmake --build --preset release --target redis_simple_benchmarks
./build/release/redis_simple_benchmarks
```

Run one group with `--benchmark_filter`, for example
`--benchmark_filter=ListIndex`. The groups cover:

- `Dict*`, `DynamicBuffer*`, `ReplyBuffer*`: core containers and buffers.
- `ZSet*`: skiplist sorted-set inserts, updates, deletes, ranks, and ranges.
- `IntsetInsert*`, `Listpack*`, `ZSetListpackInsert`, `HashListpackSet`:
  inserts into the compact encodings (and deletes, for
  `ListpackDeleteFirst`), which reallocate and move the value's bytes.
- `ListIndex`, `ListRange`: quicklist `LINDEX` and `LRANGE` at the head,
  middle, and tail of a 300,000-element list whose items vary in length. The
  middle reads step through about two quicklist nodes' worth of positions.
- `ParseCompleteRequest`, `ParseSplitRequest`, `FindCommand`: request
  decoding and command lookup.
- `AppendBulkStrings`, `FormatScore`, `AppendScores`: collection reply and
  score encoding.
- `ZSetListpackRangeByRank`: ten members by rank from a 128-member listpack
  sorted set, as `ZRANGE` and `ZREVRANGE` read them, starting at rank 0, 59,
  and 118, with scores stored as text and as integers.
- `SetIntersect`: set intersection with and without reusing stored hashes.
- `SetIntersectThree`, `SetDifference`: `SINTER` with the large set named
  before or after the one that rules members out, and `SDIFF` by probing,
  by subtracting, and by the algorithm its estimate picks.
- `DbLookup`, `DbOverwriteString`: keyspace lookups with no TTL, with a TTL
  on one key, and with a TTL on every key, and `SET` overwrites in place or
  with a new object.
- `DbSetString`: `SET` of 3-, 36-, and 100-byte values, as new keys and as
  overwrites. The sizes fall inside `std::string`'s inline buffer, inside the
  embedded-string range, and beyond both.
- `DbSetCommand`: the `SET` command's keyspace work, a lookup followed by
  the store, on new and existing keys, with and without a TTL in the
  database.
- `ActiveExpireSlowCycleNothingExpired`, `ActiveExpireReclaimExpiredKeys`:
  one active-expiration cycle over 100,000 keys none of which has expired,
  and back-to-back cycles until 100,000 expired keys are gone.
- `MultiQueue`: queueing 10 and 10,000 commands inside `MULTI`, with the
  bytes the queue holds before and after the transaction ends.

Benchmarks that take a 0 or 1 argument compare two ways of doing the same
work in one run. They are not part of CTest or CI, because timings vary
between machines. Google Benchmark's own tests are disabled in this project.

`redis_simple_benchmarks` uses the system allocator by default. To compare it
with mimalloc, which the server uses, build a second copy with
`REDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC` and run the same filter in both:

```sh
cmake -S . -B build/benchmark-mimalloc -DCMAKE_BUILD_TYPE=Release \
  -DREDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC=ON
cmake --build build/benchmark-mimalloc --target redis_simple_benchmarks
./build/benchmark-mimalloc/redis_simple_benchmarks --benchmark_filter=DbSetString
```
