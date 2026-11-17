# redis-simple

`redis-simple` is a compact Redis-compatible server in modern C++23: a TCP
server and event loop, Redis' data structures and encodings, AOF and RDB
persistence that Redis itself can load, optional threaded I/O, and a simple
CLI client. The executable is `redis_simple`.

## Requirements

- CMake 3.21+
- A C++23 compiler (GCC 13+, Clang 17+, or Apple Clang 15+)
- Git submodules for third-party dependencies
- macOS (`kqueue`) or Linux (`epoll`)

## Build

```sh
git submodule update --init --recursive
cmake --preset debug      # or: release
cmake --build --preset debug
```

Presets build into `build/<preset>`. The release preset uses link-time
optimization (`-O3 -flto`), as Redis' release build does. The server
allocates with the bundled [mimalloc](https://github.com/microsoft/mimalloc)
(`third_party/mimalloc`, v2.5.2), which replaces global `operator new` and
`delete` in the server executable only; sanitizer builds keep the default
allocator, and `-DREDIS_SIMPLE_USE_MIMALLOC=OFF` selects the system one.

## Run

```sh
./build/debug/redis_simple                      # 127.0.0.1:8080
./build/debug/redis_simple --bind 0.0.0.0 --port 6380 --requirepass s3cret
./build/debug/redis_simple --appendonly yes --appendfsync everysec
./build/debug/redis_simple --dir /var/lib/redis_simple --save "900 1 300 10"
./build/release/redis_simple --io-threads 4
./build/debug/redis_simple --help
```

Stop the server with `SIGINT` or `SIGTERM`. Any Redis client works against
it, `redis-cli -p 8080` included.

| Topic | Summary | Details |
| --- | --- | --- |
| Access | Protected mode as in Redis: without a password, only loopback clients are accepted. | [server/README.md](server/README.md) |
| Protocol | RESP2 by default, RESP3 through `HELLO 3`, inline commands, Redis' limits and error wording. | [server/README.md](server/README.md#protocol) |
| Threaded I/O | Off by default; `--io-threads N` reads and writes sockets on N-1 threads while commands stay on the main thread. | [server/README.md](server/README.md#threaded-io) |
| AOF | Off by default; Redis 8.10's multi-part format with an RDB or command base file, background rewrites, and three fsync policies. | [server/aof/README.md](server/aof/README.md) |
| RDB | `SAVE`, `BGSAVE`, and save points; files load in Redis 7.2+ and Redis' files load here. | [server/rdb/README.md](server/rdb/README.md) |
| Design | How a request flows through the code, thread handoff, and file layouts. | [docs/design.md](docs/design.md) |

## Command Coverage

Replies and errors use Redis 8's wording and number formats.

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

Not implemented: `COMMAND`, `CONFIG`, the other `CLIENT` subcommands, and
`BGREWRITEAOF` while AOF is off (Redis rewrites even then; this server
replies with an error).

Transactions, the `SCAN` family, number parsing, and lazy freeing are
described in [server/README.md](server/README.md).

## Test

```sh
ctest --preset debug                                   # everything
ctest --preset debug -L unit --output-on-failure
ctest --preset debug -L integration --output-on-failure
./build/debug/redis_simple_tests --gtest_filter=ListpackTest.*
```

Unit tests live next to the code they cover; end-to-end tests live under
`integration/`. The `release`, `sanitizer` (ASan and UBSan), and
`thread-sanitizer` presets configure, build, and test the same way:

```sh
cmake --preset sanitizer
cmake --build --preset sanitizer
ctest --preset sanitizer --output-on-failure
```

Integration tests stop the server gracefully and fail if it exits with an
error, so a race or leak reported by the server process fails its test.

| More checks | Where |
| --- | --- |
| What each integration test covers | [integration/README.md](integration/README.md) |
| Leak check, comparisons with a real Redis, formatting, clang-tidy, Linux check in Docker | [scripts/README.md](scripts/README.md) |
| libFuzzer targets | [fuzz/README.md](fuzz/README.md) |
| Microbenchmarks | [benchmarks/README.md](benchmarks/README.md) |
| Server throughput, CPU, memory, pauses, system calls | [scripts/measure/README.md](scripts/measure/README.md) |

## Project Layout

```text
benchmarks/     Google Benchmark microbenchmarks
cli/            Simple client and RESP parsing
connection/     Connection abstraction
containers/     Core in-memory data structures
data_types/     Redis hash, list, set, and sorted-set implementations
docs/           Design notes and development guide
event_loop/     Event loop with kqueue and epoll pollers
fuzz/           Stateful libFuzzer fuzzers and reference models
integration/    End-to-end tests against a running server
logging/        Project logging wrapper
scripts/        Test runners, checks against Redis, and measurement scripts
server/         Server, clients, commands, replies, and the database
server/aof/     Append-only file: writing, loading, rewriting
server/rdb/     RDB snapshots: the file format, SAVE, BGSAVE, loading
tcp/            TCP helpers
utils/          Small shared utilities
```

## Development

The code follows Google C++ style without exceptions.
[docs/development.md](docs/development.md) covers tooling, style, and CI;
`CLAUDE.md` holds the full conventions, and `.claude/skills/` the agent
skills.
