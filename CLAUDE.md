# CLAUDE.md

Guidance for AI coding agents working in this repository.

## Project Style

- Keep this a modern C++23 project. `.clang-format` stays at `c++20`, the
  newest value clang-format accepts; the formatting rules are the same.
- Follow Google C++ style and the checked-in `.clang-format`. `.clang-tidy`
  enables the `google-*` checks and Google naming: `kName` for constexpr,
  `const` static, and global constants (function-local `static const`
  included), and sized integer types (`int64_t`, `uint32_t`) instead of
  `long` or `unsigned int`, except where a system API defines the type.
- Prefer clear, simple modern C++ over legacy patterns. Use the standard
  library where it fits (`std::span`, `std::source_location`,
  `starts_with`/`ends_with`, `<bit>`, concepts, `std::ranges` algorithms
  with projections, `std::cmp_*` for mixed-sign comparisons), but avoid
  `std::format` and `std::print`, which need newer standard libraries than
  the minimum compilers in `README.md`. CI runs clang-tidy 22, which
  checks more of these than older local versions do.
- Keep naming consistent with nearby code. Name things with whole words, as
  Google style asks: `index`, `length`, `value`, `buffer`, `connection`, not
  `idx`, `len`, `val`, `buf`, `conn`. Universally known short names (`i`, `n`,
  `fd`, `it`) and Redis or POSIX terms (`backlen`, `iov_len`) are fine. In
  listpack code, `offset` is a byte position in the buffer, and `index` or
  `position` is an element's place in the list.
- Use `#define` header guards named `REDIS_SIMPLE_<PATH>_<FILE>_H_`, not
  `#pragma once`.
- Do not use exceptions. Report recoverable failures through return values
  (`bool`, `std::optional`, status enums, or `nullptr`), and guard
  programming errors with `RS_CHECK` / `utils::Unreachable()` from
  `utils/check.h`, which abort and stay enabled in release builds. Test them
  with `EXPECT_DEATH` in suites named `<Suite>DeathTest`, which the macOS leak
  check skips. The only `catch` blocks are where `std::thread` reports a
  failure to start, and the catch-all in each integration test's `main`,
  which turns an escaping standard-library exception into a failing exit
  code.
- Spell project includes from the repository root (`"server/db/db.h"`), and
  include GoogleTest as `<gtest/gtest.h>`.
- Keep objects with static storage duration trivially destructible. When one
  must hold a non-trivial type, leak it (`static T& t = *new T;`).
- Use `Create()` for owning factory functions and return `std::unique_ptr`
  instead of owning raw pointers.
- Prefer concise accessor names such as `Type()`, `Encoding()`, and
  `TotalBytes()` over Java-style `Get...` names for new or renamed APIs.
- Prefer `std::string_view` for read-only string inputs and visitor-style
  traversal for hot paths; keep vector-returning helpers for convenience APIs
  and tests, not command execution paths that can stream replies directly.
- Command handlers should read arguments through `CommandArgs`
  (`std::string_view`s into the client query buffer). Copy arguments only at an
  ownership boundary, such as storing a key/value in the DB or data structure.
- Keep request decoding incremental and zero-copy. Standard RESP arrays are the
  primary wire format; inline commands remain a compatibility path.
- Keep protocol-sensitive replies based on the client's negotiated RESP
  version. Connections default to RESP2 and may switch through `HELLO`.
- Pass `CommandArgs` by const reference instead of copying its view vector.
  Move completed `std::string` replies into `Client::AddReply`; use the
  `std::string_view` overload only for borrowed reply data.
- Add comments only when they clarify non-obvious behavior.
- Keep command handler declarations grouped in `server/commands/handlers.h`;
  avoid per-command headers unless a handler becomes a broader shared API.
- Keep command names, arity, access mode, and key positions in the sorted
  metadata table in `server/commands/command.cpp`. Preserve its allocation-free,
  case-insensitive lookup path: `Find` hashes the name with branch-free case
  folding and probes a compile-time hash index built from the table, whose
  names must be upper-case letters only and at most 16 bytes.
- When adding or changing supported commands, update the README command
  coverage list and the relevant command-family integration test.
- Keep reply encoding helpers directly under `server/`, and keep database
  state and Redis object wrappers under `server/db/`.
- Keep production and command integration servers on the shared `Server::Run`
  lifecycle. Signal handlers may only update signal-safe state; cleanup belongs
  on the event-loop thread.
- Keep AOF propagation at the successful command-mutation boundary. Preserve
  command order, encode relative TTLs as absolute `PEXPIREAT` records, and do
  not copy argument payloads beyond the owned persistence record.
- `Aof::Append` only encodes into the pending batch. The read handler calls
  `Aof::Flush` once per batch of commands, before any replies are written;
  call `Flush` before arming test failures that depend on file-write order.
  `Flush` writes the batch on the event-loop thread for every fsync policy, so
  a process crash cannot lose an acknowledged write; with `everysec` a
  separate thread only fsyncs, once a second, independently of the writes.
- After an AOF write error, `Flush` keeps the unwritten bytes and retries
  them, and write commands get `-MISCONF` until a write succeeds; a failed
  fsync keeps writes rejected until a rewrite retires the file it failed on.
  Only `always` stops the server, through `Flush` returning false.
- Reply directly after processing a client's input; install a write callback
  only while output remains unsent. Read once per readable event.
- With `--io-threads` above 1, I/O works like Redis 8: each client belongs
  to one `IoThread` (the one with the fewest clients when it connected),
  whose own event loop reads its socket and frames requests
  (`Client::ParseQueries`), then hands it to the main thread, which runs
  them (`Client::RunParsedQueries`), flushes the AOF once per handoff, and
  returns it to write the replies and resume reading. A client belongs to one
  side at a time: the I/O thread unregisters its socket events before handing
  it over, and hands clients over only after the current loop iteration, since
  connection callbacks still touch the connection after returning. Keep
  commands, the database, and the AOF on the main thread, and I/O threads to
  the client's socket, buffers, and parser. Connections close on their I/O
  thread; the main thread only frees them. Stop the I/O threads before
  freeing clients at shutdown, and pause them (`Server::PauseIoThreads`)
  around anything that must not race their allocations, such as `fork`. Run
  the threaded integration variants (`*_io_threads`) and a ThreadSanitizer
  build of the concurrent-clients test after changing this path.
- Rely on the command table for arity in handlers, and use
  `server/commands/lookup.h` for typed lookups instead of re-checking the
  object type and null database in each command.
- Prefer `Dict::FindOrInsert` for upserts so a key is hashed once and copied
  only when it is new.
- `RedisDb` operations hash their key once as an `in_memory::HashedKey` and
  pass it to the keyspace, TTL, and WATCH dicts, which share the seeded string
  hash. Keep operations that touch more than one of those dicts on that path
  instead of looking the key up by `std::string_view` in each.
- String-keyed `Dict`s hash with process-seeded SipHash-1-3
  (`utils/siphash.h`) to resist hash flooding. Do not depend on iteration or
  `SCAN` order in tests; it changes between runs.
- `Skiplist` stores keys by value, orders them with a three-way comparison,
  and searches by any probe type its comparator can compare a key against. A
  lookup probe must identify at most one key; a range-bound probe may match
  many. The sorted-set skiplist searches score ranges by a bare `double`
  bound and members by a `(score, string_view)` probe; do not reintroduce
  sentinel entries, member copies for lookups, or min/max member tracking.
  Change stored keys through `Skiplist::Update`, which repositions them.
- `ListPack::ForEach`, `ForEachReverse`, and `ForEachPair` decode each
  element once through `DecodeEntry`, which yields its value and the next
  offset together. Prefer them over `Get` plus `Next` loops on read paths.
  Listpack sorted sets read integer-encoded scores directly instead of
  formatting and reparsing them.
- Keep `QuickList` aligned with Redis' quicklist: index seeks walk nodes from
  the closer end (`Locate`), an `LSET` that overflows its node splits only
  that node, and `List` converts back to a listpack after pops, `LSET`,
  `LREM`, and `LTRIM` once one node holds at most half the size limit.
  One deliberate difference: after pops, `LSET`, `LREM`, and `LTRIM`,
  `QuickList` merges neighboring nodes that fit together, where Redis leaves
  them. This server is I/O bound, so fewer, fuller nodes are worth the merge
  cost; keep the merges.
- Canonical integer strings are stored as `int64_t`, and other values of at
  most `EmbeddedString::kCapacity` bytes (39 with libstdc++) inside the
  `RedisObject`, like Redis' embstr, so they need no allocation of their own.
  Read string objects through `RedisObject::String()` (a `StringValue` view)
  or `Int()`, and call `MutableString()` only to edit bytes in place, since it
  converts the value back to raw form. Create objects from borrowed bytes with
  `CreateWithStringCopy`, which embeds without a temporary `std::string`.
- Store whole string values through `RedisDb::SetStringKey`, which overwrites
  an existing string object in place with `RedisObject::AssignString` instead
  of allocating a new object. `AssignString` applies the same encoding rules
  as `CreateWithString`, overwrites embedded values in place, reuses a small
  or well-filled raw buffer, and releases a buffer much larger than the new
  value. It accepts a value that views the object's own bytes.
- Only commands marked `AllowBeforeAuth` in the command table run before a
  client authenticates when `--requirepass` is set.
- Inside `MULTI`, every command is queued unless the command table marks it
  `RunsImmediatelyInMulti` (`EXEC`, `DISCARD`, `MULTI`, `WATCH`, `QUIT`).
  Queued arguments are copied once into the client's transaction buffer, the
  ownership boundary for arguments that outlive a read. `EXEC` runs the queue
  under the `EXEC` command's pinned clock.
- `WATCH` invalidation lives in `RedisDb`: its key-level mutations (set,
  delete, unlink, expire, persist, rename, flush, and expiration) touch watched
  keys. Commands that edit a value in place are covered by `Client`, which
  touches a single-key command's key after it calls `MarkModified()`. Keep new
  key-level `RedisDb` mutations touching watchers, and keep
  `TouchWatchedKey` a single branch when nothing is watched.
- Wrap an `EXEC`'s AOF records with `Aof::BeginTransaction` and
  `Aof::EndTransaction`, which add `MULTI`/`EXEC` records when there is more
  than one. Replay applies a `MULTI` block only at its `EXEC` and truncates a
  block cut off at the end of the file.
- The AOF is multi-part, like Redis 7: a directory with a base file,
  incremental files, and a manifest in Redis' format (`Manifest`). A rewrite
  first switches appends to a new incremental file (`OpenNextIncrement`), so
  writes made during it need no buffer; the new base and the files it
  replaces change only through a new manifest. Keep rewrite snapshots
  visitor-based, and keep the previous manifest and files until the new
  manifest is safely renamed into place.
- `BGREWRITEAOF` forks; the child writes the snapshot and exits, and the
  server cron finishes the rewrite through `Aof::CheckRewrite`. No other
  thread may allocate or free memory during the fork: the server's fork
  hooks pause the I/O threads and the reclaimer that frees `UNLINK`ed values,
  and the fsync thread must not allocate or log. Pause any new thread that
  touches the heap the same way. Keep the child free
  of locks and logging, since another thread may have held one at the fork.
- Bound AOF snapshot memory and replay command sizes together. Split large
  strings into replay-safe commands and batch collections by encoded bytes, not
  only element count.
- Replace the manifest in this order: write and sync a temporary file, rename
  it, then sync the directory (`PersistManifest`). After a rename whose
  directory sync failed, the new manifest is live but may not survive a
  crash: adopt it, reject writes, and keep the replaced files until a later
  manifest persists safely. Delete history files only after the manifest that
  retires them is durable. Keep failed rewrites observable through
  `INFO persistence` without disabling a still-healthy AOF.
- Replay the base, then the incremental files in manifest order; only the
  last file may end in a torn record. Restarts keep appending to the last
  incremental file; only a rewrite starts a new one.

## Build And Test

Use CMake presets:

```sh
cmake --preset debug
cmake --build --preset debug
```

Run unit and integration tests separately:

```sh
ctest --preset debug -L unit --output-on-failure
ctest --preset debug -L integration --output-on-failure
```

When changing behavior shared with Redis and a reference server is available,
run:

```sh
scripts/run_redis_compatibility_check.sh build/debug/redis_simple
```

Run release and sanitizer checks after changes to ownership, memory layout,
assertions, or low-level data structures:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
cmake --preset sanitizer
cmake --build --preset sanitizer
ctest --preset sanitizer --output-on-failure
scripts/run_leak_check.sh
```

Build and run the bounded Clang libFuzzer smoke tests after changes to request
parsing, Redis data types, core containers, buffers, expiration, persistence,
or event-loop behavior:

```sh
cmake --preset fuzz
cmake --build --preset fuzz --target redis_simple_fuzzers
ctest --preset fuzz -L fuzz --output-on-failure
```

On macOS, configure the fuzz preset with Homebrew LLVM on `PATH`; Apple Clang
does not include the libFuzzer runtime.

The leak-check script uses Apple `leaks` for the macOS unit-test binary and
explicit LeakSanitizer options for the complete Linux sanitizer test suite.
Forked children stall `leaks`, so the macOS run skips death tests and
`AofTest`, whose rewrites fork; keep other forking tests out of it the same
way.

Use Docker for a local Linux build and test check from macOS:

```sh
scripts/run_linux_docker_check.sh
```

The helper prepares its cached Ubuntu toolchain without mounting the
repository. Its build and test container has networking disabled and mounts the
repository read-only.

Before committing, run the relevant build and tests.

## Test Layout

- Unit tests stay colocated with implementation files as `*_test.cpp`.
- Register unit tests in CTest by GoogleTest suite through
  `redis_simple_add_gtest_suite`, so failures identify the affected suite while
  preserving same-suite fixture behavior.
- Integration tests live under `integration/`.
- Stateful libFuzzer harnesses live under `fuzz/`, compare operations against
  simple reference models where practical, and use bounded CTest smoke runs
  for CI.
- Current integration coverage should stay focused:
  - `integration/commands/`
  - `integration/aof_client_test.cpp`
  - `integration/io_threads_client_test.cpp`
  - `integration/tcp/`
- Keep server option and graceful shutdown coverage in the lifecycle integration
  test rather than introducing a separate command-test server implementation.
- Keep AOF restart, manual and automatic rewrite compaction, shutdown flush,
  expiration, and cross-data-type recovery in the dedicated AOF integration
  test. Use injected file operations for deterministic write, sync, rename, and
  directory-sync failure unit tests.
- Keep command-family integration tests split by area, including key, string,
  set, list, zset, hash, connection, and transaction commands.
- Register integration command tests as separate CTest entries by command
  family, so failures identify the affected area without log digging.
- Keep exact ordering assertions for ordered command results such as sorted-set
  ranges; do not sort actual output in compatibility tests.
- Project runner scripts live under `scripts/`.
- Do not add manual log-inspection tests. Tests should assert behavior and
  return nonzero on failure.

## Project Management

- Run clang-format 22 on changed C/C++ files before committing or pushing. The
  format script rejects other major versions to keep local and CI output equal.
- Use `scripts/format.sh --check` and `scripts/run_clang_tidy.sh` for local
  quality checks; clang-tidy warnings are treated as errors.
- Never put required side effects inside `assert`; release builds must preserve
  behavior when assertions are disabled.
- Always update relevant docs, including `README.md` and this `CLAUDE.md`, when
  changing build, test, workflow, or project conventions. `AGENTS.md` only
  points here, so keep the guidance in this file.
- Agent skills live in `.claude/skills/<name>/SKILL.md`, and `.agents/skills`
  is a symlink to that folder, so keep one copy. Use the `pre-pr-check` skill
  before committing or opening a pull request, and update it when the checks
  in this file change.
- Keep CMake target-based. Source files are discovered by scoped directory
  globs in `CMakeLists.txt`; exclude generated, test, or entry-point sources
  explicitly when they do not belong in a library target.
- Keep event-loop pollers platform-selected in `CMakeLists.txt`: `kqueue` for
  macOS and `epoll` for Linux.
- The `redis_simple` executable links the bundled mimalloc submodule and
  replaces global `operator new`/`delete` through `mimalloc-new-delete.h`.
  Its only include sites are `server/main.cpp` and
  `benchmarks/mimalloc_new_delete.cpp`, which `memory_benchmark` builds only
  with `REDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC`. Each executable includes it at
  most once. Keep libc `malloc` unoverridden, keep sanitizer builds on the
  default allocator, and keep `REDIS_SIMPLE_USE_MIMALLOC=OFF` building.
  Allocate with `new` (containers, `std::make_unique`), never `malloc`, so
  allocations go through mimalloc.
- Measure performance changes to the server with
  `scripts/profile_server.sh` (callgrind instruction counts per workload)
  and memory changes with `scripts/measure_server_memory.sh` (RSS per key),
  passing the previous build as the baseline. Use `memory_benchmark` for
  changes to one component. Report instruction counts, not
  `redis-benchmark` throughput from a small or shared machine, which varies
  more than the effects being measured.
- Keep `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml`,
  `.clang-format`, `.clang-tidy`, and `.editorconfig` aligned with project
  conventions.
- Keep CI branch pushes limited to `main`; validate feature branches through
  pull requests targeting `main`, and cancel superseded runs for the same ref.
- Avoid unrelated refactors while making focused changes.
- Do not reintroduce stale mock targets that are not part of CTest or normal
  project workflows.
