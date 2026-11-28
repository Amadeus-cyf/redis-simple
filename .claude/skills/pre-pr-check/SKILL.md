---
name: pre-pr-check
description: Run this repository's checks before committing, pushing, or opening a pull request, choosing the extra ones the change needs (sanitizers, fuzzing, threaded I/O, Redis compatibility), and read CI failures after pushing.
---

# Pre-PR check

`CLAUDE.md` holds the rules; this is the order to run them in and how to
choose the extra checks. Fix every failure before committing, and report any
check that could not run, with the reason.

Most CI failures of past pull requests came from clang-format or clang-tidy
being left to CI, and all of the clang-tidy ones were in test or benchmark
files. Run both locally, on every changed file; section 6 lists what they
caught.

## 1. Always

1. Format the changed C/C++ files with clang-format 22:
   `scripts/format.sh`, then `scripts/format.sh --check`. The script rejects
   other major versions. If version 22 is not installed, use clang-format 21
   (Apple's Command Line Tools ship it as
   `/Library/Developer/CommandLineTools/usr/bin/clang-format`), which agrees
   with 22 on this tree, and check every file, not only the changed lines:

   ```sh
   git ls-files '*.h' '*.cpp' ':!:third_party/**' ':!:build/**' |
     xargs "${CLANG_FORMAT:?}" --dry-run --Werror
   ```

   Format with `-i` in place of `--dry-run --Werror`. Do not use a version
   older than 21, and say in the pull request that CI's check is the first
   with version 22.
2. Lint: `scripts/run_clang_tidy.sh`. Warnings are errors, and test files
   and `benchmarks/` are checked like the rest. The whole project takes a
   while; while iterating, name the changed `.cpp` files and those that
   include a changed header, then run it all once:

   ```sh
   scripts/run_clang_tidy.sh \
     $(git diff --name-only "$(git merge-base origin/main HEAD)" -- '*.cpp')
   ```

   CI runs clang-tidy 22, which flags more than older versions, but every
   check that has failed CI so far is in clang-tidy 18 too. If no
   clang-tidy is installed, ask before installing one: `brew install llvm`
   (the script finds it), or on Ubuntu the `clang-tidy` package, with
   `CLANG_TIDY_BIN` and `RUN_CLANG_TIDY_BIN` naming its binaries. Without
   one, read the diff for the patterns in section 6, and say in the pull
   request that clang-tidy did not run.
3. Build and test the debug preset:

   ```sh
   cmake --preset debug && cmake --build --preset debug
   ctest --preset debug -L unit --output-on-failure
   ctest --preset debug -L integration --output-on-failure
   ```

   Do not pass `-j` to the integration run: its servers share one port.
4. When adding a GoogleTest suite (a new first argument to `TEST` or
   `TEST_F`), register it with `redis_simple_add_gtest_suite` in
   `CMakeLists.txt`; ctest and CI run only registered suites. After the
   build, this prints each suite that is defined but not registered, and
   nothing when all are:

   ```sh
   comm -23 \
     <(build/debug/redis_simple_tests --gtest_list_tests |
       sed -n 's/^\([A-Za-z0-9_]*\)\.$/\1/p' | sort -u) \
     <(sed -n 's/^redis_simple_add_gtest_suite(\(.*\))$/\1/p' \
       CMakeLists.txt | sort -u)
   ```

5. When commands change, confirm the README command coverage list and the
   command-family integration test were updated.
6. When the request path, threaded I/O, or the AOF change, confirm
   `docs/design.md` still describes them.

## 2. Depending on what changed

| Change | Also run |
| --- | --- |
| Ownership, memory layout, assertions, low-level data structures | `release` and `sanitizer` presets (build plus `ctest`), then `scripts/run_leak_check.sh` |
| Request parsing, data types, core containers, buffers, expiration, persistence, event loop | `fuzz` preset: build `redis_simple_fuzzers`, then `ctest --preset fuzz -L fuzz`. Where the fuzz preset cannot link (no libFuzzer runtime), still compile every changed fuzzer (`clang++ -std=c++23 -fsyntax-only -I. fuzz/<name>_fuzzer.cpp`) and run it through a small `main` that calls `LLVMFuzzerTestOneInput` on a few inputs, linked with the sanitizer build's `libredis_simple_core.a`; otherwise CI is the first to build them |
| AOF (`server/aof/`) | `redis_simple_integration_aof`, `redis_simple_integration_aof_io_threads`, and `redis_simple_integration_aof_command_base` (the base file as commands), the sanitizer suite, and the `thread-sanitizer` preset's suite (the AOF background thread and the rewrite's `fork`). With a `redis-server` installed, `scripts/check_aof_interop.sh` (see the `redis-differential-check` skill), and compare behavior changes with the latest Redis `src/aof.c` |
| RDB (`server/rdb/`), or the byte format of `Listpack` or `Intset` | `redis_simple_integration_rdb` and `redis_simple_integration_rdb_io_threads`, the `RdbTest`, `RdbBackgroundSaveTest`, `ListpackTest`, `ListpackReaderTest`, and `IntsetTest` suites (which compare bytes with vectors Redis produced), the sanitizer suite, `rdb_fuzzer`, and, for `BGSAVE` or anything that forks, the `thread-sanitizer` suite. With a `redis-server` 7.2 or later, `scripts/check_rdb_interop.py` (see the `redis-differential-check` skill), and compare behavior changes with the latest Redis `src/rdb.c` |
| Threaded I/O (`server/io_thread*`, client handoff, `Client::ParseQueries`/`RunParsedQueries`) | the `*_io_threads` integration variants, and the `thread-sanitizer` preset: `cmake --preset thread-sanitizer`, build, then `ctest --preset thread-sanitizer` (serially; integration tests share a port) |
| Tests that compare times, sleep, or wait for another thread | the suite under the `sanitizer` preset, repeated: `ctest --preset sanitizer -R <Suite> --repeat until-fail:20`. Compare a time with one read of the clock: pin it (`RedisDb::PinCommandTime`) or allow a tolerance |
| Behavior shared with Redis, when a reference server is available | the `redis-differential-check` skill: `scripts/run_redis_compatibility_check.sh` and ad hoc comparisons |
| Server performance or memory, or a hot path | the `benchmark-against-baseline` skill: profile against a baseline build and report instruction counts, not throughput |
| Freeing, expiry, forking, connections, the event loop, or snapshot writing | the matching tool from the `benchmark-against-baseline` skill (`measure_pauses.py`, `count_syscalls.py`, `measure_snapshot_io.py`), against the baseline and `redis-server` |

## 3. Renames

When renaming variables, compile once with `-Wshadow-all` before and once
after, in a clean build, and compare the warnings. A renamed local can take
the name of a variable in an enclosing scope and silently change which one
the code uses, while still compiling.

When renaming or removing a function or option, search every caller, not only
the library and its tests: `fuzz/`, `benchmarks/`, `integration/`, and
`scripts/` build or run separately and are easy to miss.

## 4. Stacked pull requests

After the base pull request is squash-merged, the stacked branch conflicts
with `main`, because it still holds the base's original commits. If
`git diff <base branch tip> origin/main` is empty, `main` adds nothing the
stacked branch lacks: merge it with `git merge -s ours origin/main`, then check
that `git diff origin/main` shows only the stacked change. Otherwise resolve
the conflicts normally. Neither needs a force push.

## 5. Before pushing

- `git status` is clean apart from the intended change, and no build output
  or scratch files are staged.
- The READMEs (the root one and the one in the directory changed) and
  `CLAUDE.md` describe any changed build, test, workflow, or convention.
- The pull request lists as "not run locally" only the checks that could not
  run here, and says which fallback of section 1 ran instead.
- After pushing, read the pull request's CI when it finishes and fix
  failures (section 6); it is the only place clang-format 22, clang-tidy 22,
  and the macOS jobs all run.

## 6. When CI fails

Read the failed step's log: `gh run view <run id> --log-failed`.

- **A job fails on code this pull request does not touch.** Check the
  latest runs on `main` (`gh run list --branch main --workflow ci
  --limit 3`). If `main` fails the same way, the cause is outside this pull
  request, usually a new Redis release.
- **Redis Compatibility after a new Redis release.** Use the
  `track-redis-release` skill, in a pull request of its own. Redis 8.12
  failed "Check AOF interoperability" ("Error reading the RDB base file")
  on unrelated pull requests until the loader accepted RDB version 17.
- **Build Redis.** A failure at the link step with no other error was a race
  between two makes building `deps/` at once, which
  `scripts/build_latest_redis.sh` now avoids; the script prints the build
  log's first errors. If such a failure recurs, rerun the failed job once
  (`gh run rerun <run id> --failed`) before changing anything.
- **A test fails under one sanitizer or on one OS only.** Repeat it locally
  under that preset (`--repeat until-fail:20`) before suspecting the change.
  The one past case compared two reads of the clock, a millisecond apart
  under macOS ASan; it now pins the time.
- **Quality / Ubuntu.** These failed CI in past pull requests:

| Check | What it flagged | Fix |
| --- | --- | --- |
| clang-format 22 | `struct stat info {};`, not formatted with 21 or 22 | format it (`struct stat info{};`), or write `struct stat info = {};` as `server/aof/` does |
| `performance-inefficient-vector-operation` | `push_back` or `emplace_back` in a loop whose count is known (twice) | `reserve` before the loop |
| `bugprone-inc-dec-in-conditions` | `held > 0 && --held == 0` | decrement in a statement of its own, then test |
| `bugprone-implicit-widening-of-multiplication-result` | `48 * 1024` in a benchmark's `Args` or `ArgsProduct`, which take `int64_t` | `int64_t{48} * 1024` |
| `modernize-raw-string-literal` | an escape for a printable character, such as `"\x69"` | the character itself (`"i"`), or a raw string literal |
