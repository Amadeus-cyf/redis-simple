---
name: benchmark-against-baseline
description: Measure a change's CPU and memory cost against main (or another ref) with this repository's benchmarks, and find the functions behind any regression. Use for performance work, and before opening a pull request that touches a hot path.
---

# Benchmark against a baseline

Compare the change with a build of the code it replaces, never with numbers
remembered from an earlier run. Report instruction counts and bytes per key,
not `redis-benchmark` throughput, which varies more on a small or shared
machine than the effects being measured.

## 1. Build the baseline

Build the base ref in a separate worktree, in release mode, so the working
tree stays untouched:

```sh
base=/tmp/redis_simple_baseline   # any directory outside the repository
git worktree add --detach "${base}" origin/main
git -C "${base}" submodule update --init
(cd "${base}" && cmake --preset release && cmake --build --preset release --target redis_simple)
cmake --preset release && cmake --build --preset release --target redis_simple
```

For a stacked pull request, use its base branch instead of `origin/main`.

## 2. Measure

Pick the tools the change calls for:

| Change | Tool |
| --- | --- |
| Anything on the request path (parsing, command dispatch, replies, the database, the AOF), and RDB saving or loading | `scripts/profile_server.sh build/release/redis_simple "${base}/build/release/redis_simple"` |
| Object layout, encodings, or anything that changes memory per key | `scripts/measure_server_memory.sh build/release/redis_simple "${base}/build/release/redis_simple"` |
| One component (a container, the parser, reply encoding) | `redis_simple_benchmarks`, below |

`profile_server.sh` runs ten workloads under callgrind; `-w` picks some
(`-w pipelined-get,aof-writes`). `rdb-save-load` covers `SAVE` and loading
at startup (`server/rdb/`), and `aof-load` covers replaying an AOF. Set `PROFILE_OUTPUT_DIR` to keep the
callgrind files for step 3. A change that should not affect performance,
such as a refactor, stays within about 1% on every workload; anything more
has a cause worth finding.

For a component, build `redis_simple_benchmarks` in both trees and compare them
with Google Benchmark's tool:

```sh
cmake --build --preset release --target redis_simple_benchmarks
(cd "${base}" && cmake --build --preset release --target redis_simple_benchmarks)
python3 third_party/benchmark/tools/compare.py benchmarks \
  "${base}/build/release/redis_simple_benchmarks" build/release/redis_simple_benchmarks \
  --benchmark_filter=<Group> --benchmark_repetitions=5
```

`compare.py` needs `pip install scipy`; without it, run the same filter in
both binaries and compare the reported times by hand. Treat differences under
about 5% as noise on a shared machine.

## 3. Find the cause of a regression

Compare the two callgrind files of the workload that regressed, function by
function:

```sh
scripts/compare_callgrind.py "${PROFILE_OUTPUT_DIR}/baseline-<workload>.cg" \
  "${PROFILE_OUTPUT_DIR}/binary-<workload>.cg"
```

Patterns seen in this repository:

- A function appears with a large count and its caller shrinks by a similar
  amount: it was inlined before and is now a call, often after it grew or
  moved to another file. Check the cheap common case before calling it, or
  mark it `[[gnu::always_inline]]` (as `Client::PropagateMutation` is).
- A parser or encoder function grows on every request: a rare case joined
  the hot path. Move it into a `[[gnu::cold, gnu::noinline]]` helper, as the
  request parser does.
- `memcpy`, `malloc`, or `operator new` grows: a copy or allocation was added
  per request. Look for a temporary `std::string` or a container that is no
  longer reused.

Re-run the affected workloads after each fix (`-w`), then the full set once
at the end.

## 4. Report and clean up

Put the table from step 2 in the pull request, with the baseline named, and
explain any change over 1%. Then remove the worktree:

```sh
git worktree remove --force "${base}"
```
