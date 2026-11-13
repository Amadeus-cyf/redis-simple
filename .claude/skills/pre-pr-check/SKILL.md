---
name: pre-pr-check
description: Run this repository's checks before committing, pushing, or opening a pull request, choosing the extra ones the change needs (sanitizers, fuzzing, threaded I/O, Redis compatibility).
---

# Pre-PR check

`CLAUDE.md` holds the rules; this is the order to run them in and how to
choose the extra checks. Fix every failure before committing, and report any
check that could not run, with the reason.

## 1. Always

1. Format the changed C/C++ files with clang-format 22:
   `scripts/format.sh`, then `scripts/format.sh --check`. The script rejects
   other major versions. If version 22 is not installed, format only the
   changed lines (`git clang-format`) and say that CI's format check is the
   first version-22 check.
2. Lint: `scripts/run_clang_tidy.sh`. Warnings are errors. CI runs
   clang-tidy 22, which flags more than older local versions.
3. Build and test the debug preset:

   ```sh
   cmake --preset debug && cmake --build --preset debug
   ctest --preset debug -L unit --output-on-failure
   ctest --preset debug -L integration --output-on-failure
   ```

   Do not pass `-j` to the integration run: its servers share one port.
4. When commands change, confirm the README command coverage list and the
   command-family integration test were updated.
5. When the request path, threaded I/O, or the AOF change, confirm
   `docs/design.md` still describes them.

## 2. Depending on what changed

| Change | Also run |
| --- | --- |
| Ownership, memory layout, assertions, low-level data structures | `release` and `sanitizer` presets (build plus `ctest`), then `scripts/run_leak_check.sh` |
| Request parsing, data types, core containers, buffers, expiration, persistence, event loop | `fuzz` preset: build `redis_simple_fuzzers`, then `ctest --preset fuzz -L fuzz`. Where the fuzz preset cannot link (no libFuzzer runtime), still compile every changed harness (`clang++ -std=c++23 -fsyntax-only -I. fuzz/<harness>.cpp`) and run it through a small `main` that calls `LLVMFuzzerTestOneInput` on a few inputs, linked with the sanitizer build's `libredis_simple_core.a`; otherwise CI is the first to build them |
| AOF (`server/aof/`) | `redis_simple_integration_aof`, `redis_simple_integration_aof_io_threads`, and `redis_simple_integration_aof_command_base` (the base file as commands), the sanitizer suite, and the `thread-sanitizer` preset's suite (the AOF background thread and the rewrite's `fork`). With a `redis-server` installed, `scripts/check_aof_interop.sh` (see the `redis-differential-check` skill), and compare behavior changes with the latest Redis `src/aof.c` |
| RDB (`server/rdb/`), or the byte format of `ListPack` or `IntSet` | `redis_simple_integration_rdb` and `redis_simple_integration_rdb_io_threads`, the `RdbTest`, `RdbBackgroundSaveTest`, `ListPackTest`, `ListPackReaderTest`, and `IntSetTest` suites (which compare bytes with vectors Redis produced), the sanitizer suite, `rdb_fuzzer`, and, for `BGSAVE` or anything that forks, the `thread-sanitizer` suite. With a `redis-server` 7.2 or later, `scripts/check_rdb_interop.py` (see the `redis-differential-check` skill), and compare behavior changes with the latest Redis `src/rdb.c` |
| Threaded I/O (`server/io_thread*`, client handoff, `Client::ParseQueries`/`RunParsedQueries`) | the `*_io_threads` integration variants, and the `thread-sanitizer` preset: `cmake --preset thread-sanitizer`, build, then `ctest --preset thread-sanitizer` (serially; integration tests share a port) |
| Behavior shared with Redis, when a reference server is available | the `redis-differential-check` skill: `scripts/run_redis_compatibility_check.sh` and ad hoc comparisons |
| Server performance or memory, or a hot path | the `benchmark-against-baseline` skill: profile against a baseline build and report instruction counts, not throughput |

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
- `README.md` and `CLAUDE.md` describe any changed build, test, workflow, or
  convention.
- After pushing, watch the pull request's CI and fix failures; it is the
  only place clang-format 22, clang-tidy 22, and the macOS jobs all run.
