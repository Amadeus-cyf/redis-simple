# redis-simple

`redis-simple` is a compact Redis-compatible server implemented in modern
C++23. It includes a TCP server, event loop, command handlers, in-memory data
structures, optional append-only persistence, a simple CLI client, unit tests,
integration tests, and memory benchmarks. The executable remains
`redis_simple`.

The project is intentionally compact: implementation-level unit tests live next
to the code they cover, while runnable end-to-end checks live under
`integration/`.

## Requirements

- CMake 3.21+
- A C++23 compiler (GCC 13+, Clang 17+, or Apple Clang 15+)
- Git submodules for third-party dependencies
- macOS (`kqueue`) or Linux (`epoll`) for the event-loop poller

## Setup

```sh
git submodule update --init --recursive
cmake --preset debug
cmake --build --preset debug
```

The debug preset builds into `build/debug`. A release preset is also available:

```sh
cmake --preset release
cmake --build --preset release
```

The server allocates its objects with the bundled
[mimalloc](https://github.com/microsoft/mimalloc) (`third_party/mimalloc`,
v2.5.2). It replaces global `operator new` and `delete` in the server
executable only, not libc `malloc`, so it behaves the same on Linux and macOS.
Sanitizer builds keep the default allocator. Configure with
`-DREDIS_SIMPLE_USE_MIMALLOC=OFF` to use the system allocator.

## Run

Start the server:

```sh
./build/debug/redis_simple
```

The server binds to `127.0.0.1:8080` by default. Configure either value at
startup and stop cleanly with `SIGINT` or `SIGTERM`:

```sh
./build/debug/redis_simple --bind 0.0.0.0 --port 6380 --requirepass s3cret
./build/debug/redis_simple --help
```

Like Redis, the server starts in protected mode: when no password is set, it
only accepts clients connecting over loopback and answers others with a
`DENIED` error. Set a password with `--requirepass`, after which clients must
send `AUTH <password>` (or `AUTH default <password>`, or `HELLO <protover>
AUTH default <password>`) before other commands, or disable the check with
`--protected-mode no` on a trusted network.

Client sockets are set up as Redis sets up its own: Nagle's algorithm is off
(`TCP_NODELAY`), so a reply is not held back for the acknowledgment of the
one before it, and the kernel probes a peer that has been silent for 300
seconds (Redis' default `tcp-keepalive`), so a client whose machine vanished
does not keep its connection forever. The interval is not configurable.

Enable AOF persistence with Redis-style fsync policies:

```sh
./build/debug/redis_simple --appendonly yes \
  --appenddirname appendonlydir --appendfilename appendonly.aof \
  --appendfsync everysec \
  --auto-aof-rewrite-min-size 67108864 \
  --auto-aof-rewrite-percentage 100
```

AOF is disabled by default. When enabled, it follows Redis 8.10's multi-part
AOF (`src/aof.c`), and Redis loads its files and it loads Redis', with the
base file in either of Redis' formats. The AOF is a directory
(`--appenddirname`, default `appendonlydir`) holding one base file, the
snapshot of the last rewrite, then incremental files with the mutations since,
and a manifest that lists them in Redis' format; base and incremental files
are numbered separately:

```
file appendonly.aof.2.base.rdb seq 2 type b
file appendonly.aof.2.incr.aof seq 2 type i
```

As in Redis (`--aof-use-rdb-preamble yes`, the default), the base file is an
RDB snapshot (`.base.rdb`, following `--rdbcompression` and `--rdbchecksum`),
which is smaller and faster to write and load than commands; with
`--aof-use-rdb-preamble no` it holds commands (`.base.aof`). Loading reads
either by its content, whatever the option says, as Redis does: a file that
starts with `REDIS` is read as RDB, keeping keys whose time has passed until
loading ends, as the commands after it expect, and then any commands after
it, so an old single-file AOF with an RDB preamble loads too. On 770,900 keys
of every type, the RDB base was 2.6 times smaller (46 MB against 120 MB),
written 1.8 times as fast, and loaded about twice as fast.

`--appendfilename` (default `appendonly.aof`) is the files' name prefix, a
name without a directory as in Redis; the manifest quotes names that need it.
`--appenddirname` is relative to `--dir`, and unlike Redis also accepts a
path. An AOF with no files starts with a base file of the
current dataset. An existing single-file AOF of that name in the working
directory is upgraded as Redis does: listed as the base file in a new
manifest, then moved into the directory. The manifest is replaced atomically:
written to a temporary file, synced, renamed, and the directory synced.

Mutations are encoded into a buffer that is written before any of their replies
are sent, as in Redis. `everysec`, the default, syncs on a background thread
once a second; as in Redis, a write waits up to two seconds while that sync
still runs (`aof_delayed_fsync` counts the times it stopped waiting), so an
operating-system crash can cost about the last two seconds. `always` syncs each
batch (group commit), and `no` leaves syncing to the operating system. Graceful
shutdown writes and syncs everything pending. `--no-appendfsync-on-rewrite yes`
skips syncs while a rewrite or a background save runs, and `--aof-timestamp-enabled yes` writes
Redis' `#TS:` annotations, which loading skips.

`BGREWRITEAOF` compacts command history. It first switches appends to a new
incremental file and lists it in the manifest, then forks a child that writes
a snapshot of the database, from its copy-on-write view of memory, as
`temp-rewriteaof-bg-<pid>.aof`, syncing every 4 MiB
(`--aof-rewrite-incremental-fsync`); the server keeps serving meanwhile. In
the command format, large strings are split into replay-safe commands, and
collection batches are limited by encoded bytes. When the child finishes, its file becomes the next
base file, and a new manifest lists it with the incremental file opened at the
start. The old base and incremental files are then deleted on a background
thread, as Redis' bio thread does. Writes made during the rewrite are already
in that incremental file, so nothing is buffered or copied at the end. Inside
`MULTI`, `BGREWRITEAOF` is scheduled and the cron starts it after `EXEC`.
Shutdown kills a rewrite that has not been installed yet; the previous files
stay listed.

Replay loads the base, then each incremental file in order; a listed file that
is missing stops startup. Only the last file may end in a torn record, which is
cut off unless `--aof-load-truncated no`. `--aof-load-corrupt-tail-max-size`
(default 0, off) also cuts off a format error with fewer than that many bytes
after the last complete command.

Automatic rewriting starts once the AOF is larger than 64 MiB and has grown by
100% from its size after the last rewrite (or its base file's at startup). The
two `--auto-aof-rewrite-*` options override those defaults; a percentage of `0`
disables automatic rewriting. After three consecutive failed rewrites,
automatic and scheduled rewrites wait one minute, then two, four, and so on up
to an hour, as Redis does; `BGREWRITEAOF` still starts one at once. `INFO
persistence` reports Redis' AOF fields.

When the file cannot be written, as on a full disk, the server cuts a partial
write back off the file, keeps the buffer, and retries it; until a write
succeeds, it rejects write commands, and `PING`, with Redis' `-MISCONF Errors
writing to the AOF file` error while reads keep working; `EXEC` with queued
writes discards the transaction with `-EXECABORT`. A failed background fsync rejects
writes the same way until the next fsync succeeds. With `always`, which
promises that every acknowledged write is synced, a failed write or sync stops
the server instead.

Two failure cases differ from Redis on purpose, where Redis can lose data. When
a new manifest is renamed into place but syncing the directory fails, Redis
deletes the new files the live manifest lists; this server keeps them, rejects
writes, and keeps the replaced files until a later manifest persists safely.
And a rewrite does not start while a failed write is still pending, since
Redis would then replay those commands twice.

RDB snapshots follow Redis 8.10's `src/rdb.c`. `SAVE` writes the database to
`dump.rdb` (`--dbfilename`) in the working directory (`--dir`, which also holds
the AOF): first as `temp-<pid>.rdb`, synced, then renamed over the file with
the directory synced, so a crash never leaves a partial file in its place.
`BGSAVE` does the same in a forked child while the server keeps serving. As
in Redis, one child runs at a time: `BGSAVE SCHEDULE` waits for a running AOF
rewrite, `BGREWRITEAOF` waits for a running save, and inside `MULTI` both are
scheduled for after the transaction. `LASTSAVE` reports when the last save
finished. At startup, when the AOF is off, the server loads the file if
there is one, and refuses to start if it is damaged.

Save points start background saves on their own, as Redis' `save` does:
`--save "<seconds> <changes> ..."` saves once that many changes were made and
more than that many seconds have passed since the last save. The default is
Redis 8's, `3600 1 300 100 60 10000`. The first `--save` replaces it and later
ones add pairs; `--save ""` turns saving off. Changes are counted as Redis
counts them (one per key, member, field, or element set or removed), and
`INFO persistence` reports them with Redis' other `rdb_` fields. With save
points, the server also saves when it shuts down, and keeps running if that
save fails. After a failed background save, write commands and `PING` are
refused with Redis' `MISCONF` error until a save succeeds, unless
`--stop-writes-on-bgsave-error no`.

```sh
./build/debug/redis_simple --dir /var/lib/redis_simple --dbfilename dump.rdb \
  --save "900 1 300 10"
```

The file is in Redis' format, and each server loads the other's. Values are
stored in the encodings Redis stores them in, listpacks and intsets byte for
byte as they are in memory; strings over 20 bytes are LZF-compressed when
that saves space (`--rdbcompression`), and the file ends with a CRC64
(`--rdbchecksum`). It is written as RDB version 11, the oldest that covers
its contents, so Redis 7.2 and later load it; Redis 8.10 writes version 15.
This server loads files from Redis 7.0 and later, 8.10's included, and the
plain encodings of older versions. It refuses what it does not have: streams,
modules, hash field expiration, hash templates, keys outside database 0, and
the ziplist encodings of Redis 6.2 and earlier; it skips function libraries
with a warning. Keys whose time has passed are dropped while loading. Every
length, encoded listpack and intset, and duplicate is checked, so a corrupt
file is refused, never trusted. As in Redis, a set, hash, or sorted set
listpack that passes those checks and fits this server's limits is kept as
stored, so loading it is a copy rather than one insert per element.

Clients can send standard RESP arrays of bulk strings or the legacy inline
syntax, both framed as Redis frames them: inline arguments may be quoted and
escaped as in `redis-cli`, empty lines and empty arrays are skipped, and
malformed requests get Redis' `Protocol error` replies before the connection
closes. Bulk strings may hold up to 512 MiB (Redis' `proto-max-bulk-len`), the
request buffer up to 1 GiB (`client-query-buffer-limit`), and a client that
has not authenticated may send at most 10 arguments of 16 KiB each, as in
Redis. RESP2 is the default reply protocol; `HELLO 3` switches a connection to
RESP3, including native null, map, set, and double replies. Pending output pauses reads at 8 MiB, resumes below 4 MiB,
and closes the connection before exceeding 64 MiB.

Threaded I/O, like Redis 8's `io-threads`, is off by default:

```sh
./build/release/redis_simple --io-threads 4
```

The count includes the main thread, from 1 (the default) to 128, so
`--io-threads 4` starts three I/O threads. Each runs its own event loop, and
each new connection goes to the I/O thread with the fewest clients and stays
there. For its clients, an I/O thread:

1. Reads the socket and frames the complete requests, without running them.
2. Hands the client to the main thread, which runs the commands in order, so
   commands and data structures stay single-threaded, and flushes the AOF
   once for all the clients handed over together.
3. Gets the client back, writes the replies, and resumes reading.

A client belongs to one side at a time, and mutex-guarded queues with pipe
wakeups carry it between them, so no thread spins waiting for another.
Closed connections are closed on their I/O thread and freed on the main
thread. `INFO stats` reports `io_threads_active`,
`io_threaded_reads_processed`, and `io_threaded_writes_processed`, and
`INFO threads` lists each I/O thread's clients, reads, and writes.

As with Redis, threads pay off only when the main thread is the bottleneck,
as with heavily pipelined traffic on a machine with spare cores; without
pipelining, round trips limit throughput and the default of 1 is as fast.

In another terminal, run an integration client or your own TCP client against
`localhost:8080`.

## Command Coverage

The server supports Redis-style RESP replies for the implemented command set.
Error replies use Redis 8's wording, such as
`ERR wrong number of arguments for 'get' command` and `WRONGTYPE`.

Numbers follow Redis 8: integer arguments must be canonical (no `+`, leading
zeros, or `-0`), scores and other floats parse like `strtod` (so `inf` and
hex work) while range bounds also take overflow, and scores print as Redis 8
prints them: integers up to 2^62 in full, otherwise the shortest digits in
`fpconv_dtoa`'s layout, such as `0.00001`, `1e-7`, or `6.948674738744653e+5`.

- Keys: `DEL`, `UNLINK`, `EXISTS`, `TYPE`, `EXPIRE`, `EXPIREAT`, `PEXPIRE`,
  `PEXPIREAT` with `NX`, `XX`, `GT`, and `LT` (and `XX` combined with `GT` or
  `LT`), `TTL`, `PTTL`, `PERSIST`, `RENAME`, `DBSIZE`, `FLUSHDB [ASYNC|SYNC]`
  (always synchronous), `SCAN` with `MATCH`, `COUNT`, and `TYPE`
- Strings: `GET`, `SET` with `NX`, `XX`, `GET`, `EX`, `PX`, `EXAT`, `PXAT`,
  and `KEEPTTL`, `INCR`, `INCRBY`, `DECR`, `DECRBY`, `APPEND`, `STRLEN`,
  `MGET`, `MSET`
- Lists: `LPUSH`, `RPUSH`, `LPOP` and `RPOP` with a count, `LLEN`, `LRANGE`,
  `LINDEX`, `LSET`, `LREM`, `LTRIM`
- Sets: `SADD`, `SCARD`, `SREM`, `SMEMBERS`, `SISMEMBER`, `SINTER`, `SUNION`,
  `SDIFF`, `SSCAN` with `MATCH` and `COUNT`
- Sorted sets: `ZADD` with `NX`, `XX`, `GT`, `LT`, `CH`, and `INCR`, `ZCARD`,
  `ZREM`, `ZRANK` and `ZREVRANK` with `WITHSCORE`, `ZRANGE` with `BYSCORE`,
  `BYLEX`, `REV`, `LIMIT`, and `WITHSCORES`, `ZREVRANGE`, `ZRANGEBYSCORE`,
  `ZREVRANGEBYSCORE`, `ZRANGEBYLEX`, `ZREVRANGEBYLEX`, `ZLEXCOUNT`, `ZCOUNT`,
  `ZSCORE`, `ZSCAN` with `MATCH` and `COUNT`
- Hashes: `HSET`, `HMSET`, `HGET`, `HDEL`, `HLEN`, `HEXISTS`, `HGETALL`,
  `HMGET`, `HKEYS`, `HVALS`, `HINCRBY`, `HSCAN` with `MATCH`, `COUNT`, and
  `NOVALUES`
- Transactions: `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH`
- Persistence and stats: `BGREWRITEAOF`, `SAVE`, `BGSAVE`, `LASTSAVE`,
  `INFO [section ...]` (`persistence`, `stats`, `threads`, or `all`)
- Connection: `AUTH`, `HELLO` with RESP2 and RESP3 negotiation, `AUTH`, and
  `SETNAME`, `CLIENT ID|GETNAME|SETNAME|SETINFO`, `SELECT 0` (the only
  database), `PING`, `ECHO`, `QUIT`

The `SCAN` family iterates as Redis 8 does. Each step of a hash-table scan
visits buckets until it has seen `COUNT` elements, or ten times `COUNT`
buckets, before filtering by `MATCH` or `TYPE`. A collection still in a
compact encoding (a listpack or intset) comes back whole in one step with
cursor 0. Collections use Redis 8's default encoding limits: hashes stay
listpacks up to 512 entries, sets and sorted sets up to 128 (sets of integers
stay intsets up to 512), and each element up to 64 bytes. So `HGETALL`, the
other whole-collection replies, and `*SCAN` return elements in the same
order as Redis while the collection is compact.

Not implemented: `COMMAND`, `CONFIG`, the other `CLIENT` subcommands, and
`BGREWRITEAOF` while AOF is off (Redis rewrites even then; this server
replies with an error).

Transactions follow Redis semantics. After `MULTI`, commands are queued and
answered with `QUEUED`; `EXEC` runs them back to back, with no other client's
command in between, and returns their replies as one array. A command that
fails at runtime reports its error in that array without stopping the rest. A
command rejected while queueing (unknown name, wrong arity, or `NOAUTH`) makes
`EXEC` fail with `EXECABORT`. `WATCH` provides optimistic locking: if a watched
key is written, deleted, renamed, flushed, or expires before `EXEC`, `EXEC`
returns a null reply and runs nothing. `EXEC` and `DISCARD` unwatch every key,
and one transaction may queue at most 64 MiB of arguments.

`UNLINK` detaches keys synchronously and releases their values on a background
worker. Command names, arity, access mode, and key positions are held in one
allocation-free registry used for case-insensitive dispatch and early argument
validation.

When enabled, AOF records successful mutations as canonical RESP commands and
replays them before the server accepts clients. An `EXEC` that produces more
than one record is written between `MULTI` and `EXEC` records, so replay
applies the transaction as a whole; a transaction cut off at the end of the
file is truncated rather than partially applied. Relative expirations are stored
as absolute millisecond timestamps, incomplete final records are truncated,
and corruption before the final record prevents startup. AOF rewrites traverse
collection values through borrowed views in a forked child, and keep the
previous manifest and files if rewriting fails before the new manifest is
renamed into place.

## Test

Run the full test suite through CTest:

```sh
ctest --preset debug
```

Run unit and integration tests separately:

```sh
ctest --preset debug -L unit --output-on-failure
ctest --preset debug -L integration --output-on-failure
```

Release and sanitizer presets are available for checking behavior with
assertions disabled and for detecting memory or undefined-behavior defects:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure

cmake --preset sanitizer
cmake --build --preset sanitizer
ctest --preset sanitizer --output-on-failure
```

The `thread-sanitizer` preset runs the whole suite under ThreadSanitizer, to
find data races between the event loop, the I/O threads, and background work:

```sh
cmake --preset thread-sanitizer
cmake --build --preset thread-sanitizer
ctest --preset thread-sanitizer --output-on-failure
```

Integration tests stop the server gracefully and fail if it exits with an
error, so a race or leak reported by the server process fails its test.

Run the platform-specific memory leak check after building the corresponding
preset:

```sh
scripts/run_leak_check.sh
```

On macOS, the script runs the debug unit-test binary under Apple's `leaks`
tool because Apple AddressSanitizer does not provide LeakSanitizer. It skips
death tests and `AofTest`, whose forked children stall `leaks`. On
Linux, it runs the complete sanitizer CTest suite with LeakSanitizer explicitly
enabled.

Build the opt-in Clang libFuzzer targets and run their bounded CTest smoke
checks with:

```sh
cmake --preset fuzz
cmake --build --preset fuzz --target redis_simple_fuzzers
ctest --preset fuzz -L fuzz --output-on-failure
```

The fuzz targets exercise incremental RESP parsing; listpack, quicklist, Dict,
Skiplist, and Intset mutation; Redis list, set, hash, and sorted-set behavior;
dynamic and reply buffers; database expiration; and deterministic event-loop
callbacks. AOF replay also has a bounded malformed-input target. Every target
runs under AddressSanitizer and
UndefinedBehaviorSanitizer. On macOS, put Homebrew LLVM on `PATH` before
configuring because Apple Clang does not ship a libFuzzer runtime:

```sh
brew install llvm
PATH="$(brew --prefix llvm)/bin:${PATH}" cmake --preset fuzz
```

This runs:

- `redis_simple_unit_<SuiteName>`: unit tests compiled into
  `redis_simple_tests` and registered in CTest by GoogleTest suite.
- `redis_simple_integration_tcp`: TCP client/server integration check.
- `redis_simple_integration_server_lifecycle`: command-line option, nondefault
  bind, and graceful shutdown checks.
- `redis_simple_integration_aof`: production-server append, background rewrite,
  compaction, shutdown, restart, expiration, and cross-data-type recovery checks.
- `redis_simple_integration_aof_io_threads`: the same against a server with
  `--io-threads 4`, so rewrites fork while I/O threads run.
- `redis_simple_integration_aof_command_base`: the same with
  `--aof-use-rdb-preamble no`, so the base file holds commands.
- `redis_simple_integration_command_connection`: connection command integration
  checks.
- `redis_simple_integration_command_key`: generic key command integration
  checks.
- `redis_simple_integration_command_string`: string command integration checks.
- `redis_simple_integration_command_set`: set command integration checks.
- `redis_simple_integration_command_list`: list command integration checks.
- `redis_simple_integration_command_zset`: sorted-set command integration
  checks.
- `redis_simple_integration_command_hash`: hash command integration checks.
- `redis_simple_integration_command_transaction`: `MULTI`/`EXEC` and `WATCH`
  integration checks across two connections.
- `redis_simple_integration_command_*_io_threads`: each command family again,
  against a server started with `--io-threads 4`.
- `redis_simple_integration_concurrent_clients_plain` and
  `..._io_threads`: 32 concurrent pipelined clients, with replies, totals,
  and `INFO stats` checked. The threaded variant requires batches shared
  across the I/O threads.

For debugging a single executable directly:

```sh
./build/debug/redis_simple_tests
./build/debug/redis_simple_integration_set_client
```

For debugging one unit-test suite directly:

```sh
./build/debug/redis_simple_tests --gtest_filter=ListpackTest.*
```

The integration clients return nonzero on failed expectations, so they are safe
to run in CI instead of relying on manual log inspection.

When a reference Redis server is available on port 6379, run the focused
differential protocol and command check with:

```sh
scripts/run_redis_compatibility_check.sh build/debug/redis_simple
```

The script uses `redis-cli` against both servers and covers RESP requests,
spaces in arguments, expiration replies, sorted-set range semantics, and
transaction replies.

To compare every supported command's replies, edge cases and seeded random
operations included, in RESP2 and RESP3 (the script starts both servers):

```sh
scripts/compare_with_redis.py build/debug/redis_simple
```

To compare what replies do not show, each write command's change count and
whether it fails another client's `WATCH`:

```sh
scripts/compare_side_effects.py build/debug/redis_simple
```

With `redis-server` 7 or later installed, check that each server loads the
other's AOF directory:

```sh
scripts/check_aof_interop.sh build/debug/redis_simple
```

It runs every write command this server supports, with a rewrite in
between, loads the directory in the other server, and compares the
keyspaces. Set `INTEROP_KEEP=1` to keep the directories for inspection.

With `redis-server` 7.2 or later, check that each server loads the other's
RDB file:

```sh
scripts/check_rdb_interop.py build/debug/redis_simple
```

It fills one server with every type in each of its encodings, with edge
cases such as entries at listpack size limits, saves, loads the file in the
other server, and compares every key, in both directions and without Redis'
compression and checksum.

## Benchmarks

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
- `ListIndex`, `ListRange`: quicklist `LINDEX` and `LRANGE` at the head,
  middle, and tail of a 300,000-element list.
- `ParseCompleteRequest`, `ParseSplitRequest`, `FindCommand`: request
  decoding and command lookup.
- `AppendBulkStrings`, `FormatScore`, `AppendScores`: collection reply and
  score encoding.
- `SetIntersect`: set intersection with and without reusing stored hashes.
- `DbLookup`, `DbOverwriteString`: keyspace lookups with and without a TTL,
  and `SET` overwrites in place or with a new object.
- `DbSetString`: `SET` of 3-, 36-, and 100-byte values, as new keys and as
  overwrites. The sizes fall inside `std::string`'s inline buffer, inside the
  embedded-string range, and beyond both.

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

### Server profiling

Two scripts measure the whole server under load generated by
`redis-benchmark`. Each takes a server binary and, optionally, a baseline
binary to compare against:

```sh
# Instructions executed per workload, with callgrind (Linux, needs valgrind)
scripts/profile_server.sh build/release/redis_simple path/to/baseline/redis_simple
# Resident memory per key after loading 3-, 36-, and 100-byte values
scripts/measure_server_memory.sh build/release/redis_simple path/to/baseline/redis_simple
```

`profile_server.sh` runs eleven workloads: a small command mix, pipelined and
unpipelined `GET`/`SET`, 4 KB and 36-byte values, 1,000-member collection
reads, writes with AOF on, keys with TTLs plus `SCAN`, saving and loading
an RDB file of every type, replaying an AOF, and editing lists by
position. Pick some with
`-w`, for example `-w unpipelined,aof-writes`. It counts instructions rather
than time because counts barely vary between runs, so differences of a few
percent are reliable even on a busy or small machine. Counts miss cache-miss
costs, so confirm large changes with `redis-benchmark` throughput on a quiet
machine. Set `PROFILE_OUTPUT_DIR` to keep the callgrind files, which
`callgrind_annotate` breaks down by function.

`scripts/compare_callgrind.py BASELINE.cg BINARY.cg` lists the functions
whose instruction counts changed most between two such files, which shows
where a regression comes from.

`measure_server_memory.sh` reports idle and loaded RSS and bytes per key.
`MEMORY_VALUE_SIZES`, `MEMORY_REQUESTS`, and `MEMORY_KEY_RANGE` change the
load. Both scripts need `redis-benchmark` and `redis-cli`, which ship with
Redis (`brew install redis` or `apt install redis-tools`). Like the
benchmarks, they are not part of CTest or CI.

## Project Layout

```text
cli/                  Simple client and RESP parsing
connection/           Connection abstraction
docs/                 Design notes (docs/design.md)
event_loop/           Event loop with kqueue and epoll pollers
fuzz/                 Stateful libFuzzer harnesses and reference models
integration/commands/ Server/client command integration tests
integration/aof_client_test.cpp  AOF restart integration client
integration/rdb_client_test.cpp  RDB SAVE, BGSAVE, save point, and restart client
integration/io_threads_client_test.cpp  Concurrent-clients integration client
integration/tcp/      TCP client/server integration tests
logging/              Project logging wrapper
containers/           Core in-memory data structures
scripts/              Project automation and CTest runner scripts
server/               Server, client connection glue, commands, replies, DB
server/aof/           Append-only file: writing, loading, rewriting
server/rdb/           RDB snapshots: the file format, SAVE, BGSAVE, loading
data_types/           Redis hash, list, set, and sorted-set implementations
tcp/                  TCP helpers
utils/                Small shared utilities
benchmarks/           Google Benchmark microbenchmarks
```

Command handler declarations are grouped in `server/commands/handlers.h`; the
individual command `.cpp` files keep implementation details local. Handlers
rely on the command table for arity and use `server/commands/lookup.h` for
typed key lookups that report `WRONGTYPE`.
Reply encoding helpers live directly under `server/`, while database state and
Redis object wrappers stay under `server/db/`, the append-only file under
`server/aof/`, and RDB snapshots under `server/rdb/`. Commands reach server-wide state (the database, the AOF, the
password, the I/O threads) through the client's `ServerContext`.
`docs/design.md` explains how a request flows through these parts, how
threaded I/O hands clients between threads, and how the AOF and RDB files
are organized.

## Tooling

Project management files are checked in:

- `CMakePresets.json` for reproducible configure/build/test commands.
- `.clang-format` for Google-style formatting with clang-format 22.
- `.clang-tidy` for static-analysis defaults, including the `google-*` checks
  and Google naming rules (`kConstant` names for constants and `const`
  statics, `member_` suffixes for private members).
- `.editorconfig` for editor consistency.
- `.github/workflows/ci.yml` for CI on push and pull request.
- `CLAUDE.md` for AI coding-agent project guidance; `AGENTS.md` points to it
  for agents that look for that file.
- `.claude/skills/` for agent skills, such as `pre-pr-check`, which runs the
  checks a change needs before a pull request. `.agents/skills` links to the
  same folder for agents that look there.

Format project C++ files with:

```sh
cmake --build --preset debug --target format
```

Formatting is pinned to clang-format 22 so local and CI output remains stable.
Install it with `brew install llvm` on macOS or `apt install clang-format-22`
on Ubuntu. `scripts/format.sh` discovers those installations and honors
`CLANG_FORMAT_BIN` when it points to a clang-format 22 executable.

Check formatting without modifying files with:

```sh
scripts/format.sh --check
```

Run clang-tidy with the project wrapper:

```sh
scripts/run_clang_tidy.sh
```

The wrapper uses `build/debug/compile_commands.json` by default and discovers
Homebrew's LLVM toolchain on macOS. Set `BUILD_DIR`, `CLANG_TIDY_BIN`, or
`RUN_CLANG_TIDY_BIN` to override those defaults.

The root `CMakeLists.txt` is target-based. Production, test, and benchmark
sources are discovered from scoped project directories with
`GLOB_RECURSE CONFIGURE_DEPENDS`, while test files and executable entry points
are excluded from library targets explicitly. Platform-specific event-loop
pollers are selected at configure time: `kqueue` on macOS and `epoll` on Linux.

Run the Linux build and test flow locally from macOS through Docker:

```sh
scripts/run_linux_docker_check.sh
```

On its first run, the Docker helper creates a local Ubuntu 24.04 toolchain image
without mounting the repository. Build and test runs then disable container
networking, mount the repository read-only, drop Linux capabilities, and build
under a temporary filesystem. This validates the Linux `epoll` poller without
writing Linux artifacts into the working tree or exposing source code to the
container network. Set `DOCKER_BASE_IMAGE`, `DOCKER_TOOLCHAIN_IMAGE`, or
`LINUX_DOCKER_BUILD_DIR` to override the defaults.

## C++ Style

The code follows Google C++ style with project-local conventions: owning factory
functions are named `Create()` and return `std::unique_ptr`, while concise
accessors such as `Type()`, `Encoding()`, and `TotalBytes()` are preferred over
Java-style `Get...` names.

As in Google style, headers use `#define` guards, includes are spelled from the
repository root, and the code does not use exceptions. Recoverable failures are
returned as values (`bool`, `std::optional`, status enums, or `nullptr`), while
programming errors are caught by `RS_CHECK` in `utils/check.h`, which aborts
and stays enabled in release builds. Objects with static storage duration are
trivially destructible or deliberately leaked.

Hot paths prefer `std::string_view` for read-only inputs and visitor-style
traversal when callers can encode or rebuild results directly. Vector-returning
helpers remain useful for convenience APIs and tests, but command handlers
should avoid materializing extra copies when they can stream replies in one
pass.

Command handlers borrow their argument views directly from the client query
buffer. Completed string replies are moved into the output queue, and the queue
reuses scatter/gather metadata across partial writes.

## CI

CI runs for pull requests targeting `main` and for pushes to `main`. A new run
for the same pull request or branch cancels any older run still in progress,
avoiding duplicate branch-push runs and work on superseded commits.

GitHub Actions runs debug, release, and ASan/UBSan matrices on macOS and Ubuntu
26.04, so both event-loop pollers are built and tested. macOS also runs Apple
`leaks`; Ubuntu runs LeakSanitizer, ThreadSanitizer, clang-format 22,
clang-tidy 22, and bounded libFuzzer smoke tests. A separate Ubuntu job builds the latest
stable Redis release (`scripts/build_latest_redis.sh`) and compares
supported behavior with it: replies, change counts and `WATCH`, and AOF and
RDB files in both directions. Locally, the first of those checks is:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -L unit --output-on-failure
ctest --preset debug -L integration --output-on-failure
```
