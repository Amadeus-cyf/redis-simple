---
name: redis-differential-check
description: Compare this server's behavior with a real redis-server, including AOF interoperability in both directions. Use after changing command behavior, replies, request parsing, or the AOF, when redis-server is installed.
---

# Differential check against Redis

The target is the latest Redis release (see `CLAUDE.md`). The installed
`redis-server` may be older, so first check its version and keep the known
differences below in mind instead of "fixing" them.

```sh
redis-server --version
cmake --preset debug && cmake --build --preset debug --target redis_simple
```

When the installed server is older than the latest release, build that
release from source and pass it to the scripts; it needs no install:

```sh
git clone --depth 1 --branch 8.10.3 https://github.com/redis/redis.git
make -C redis -j"$(nproc)" BUILD_TLS=no MALLOC=libc redis-server redis-cli
scripts/compare_with_redis.py build/debug/redis_simple \
  --redis-server redis/src/redis-server
scripts/check_aof_interop.sh build/debug/redis_simple redis/src/redis-server
```

## 1. Command compatibility script

`scripts/run_redis_compatibility_check.sh` compares replies with a reference
server, but does not start one, and starts this server on its default port
8080, which must be free:

```sh
redis-server --port 6379 --save '' --appendonly no --daemonize yes
scripts/run_redis_compatibility_check.sh build/debug/redis_simple
redis-cli -p 6379 shutdown nosave
```

Profiling (`scripts/profile_server.sh`, ports from 8200) and the memory
script (8500) use other ports and can run at the same time.

## 2. Every command, edge cases and random operations

`scripts/compare_with_redis.py` starts both servers itself and compares
replies to about 1,900 fixed edge cases (boundary indices and counts,
invalid numbers, wrong types and arities, option combinations) and to seeded
random operations whose collections grow past their compact encodings, in
RESP2 and RESP3:

```sh
scripts/compare_with_redis.py build/debug/redis_simple
scripts/compare_with_redis.py build/debug/redis_simple --io-threads 3 --seeds 1
```

It exits nonzero and prints each differing command when replies differ. Add
cases for new commands and options to its `edge_cases` and
`random_operations`. Against Redis 7 it compares non-integer numbers by
value, since only their formatting changed in Redis 8.

For a one-off question, send the same commands to both servers with
`redis-cli` and diff the replies. Clear data with `FLUSHDB`, since this
server has no `FLUSHALL`, and do not compare `SCAN`, `SMEMBERS`, or
`HGETALL` order, which depends on the hash seed.

## 3. AOF interoperability

After any AOF change, check that each server loads the other's AOF directory:

```sh
scripts/check_aof_interop.sh build/debug/redis_simple
```

It runs every write command this server supports, with its options, with a
rewrite in between, so the directory holds a base file and an incremental
file, then loads it in the other server and compares the keyspaces. Add new
write commands to its list. Its command list also makes a quick reply
comparison: extract it and send it to both servers in RESP2 and RESP3. Set
`INTEROP_KEEP=1` to keep the directories when it fails. A failure to load
usually means the other server wrote a command this one does not implement:
Redis' rewrite writes `SET`, `RPUSH`, `SADD`, `ZADD`, `HMSET`, and
`PEXPIREAT`.

## Known differences

Not bugs; leave them unless the task is about them.

- Versions before 8: scores and other doubles print with `%.17g`, where
  Redis 8 (and this server) uses `d2string`'s shortest form; `SCAN` accepts
  invalid cursors; `ZRANK`/`ZREVRANK` have no `WITHSCORE` before 7.2.
  Redis 7 also ends every unknown-command error with
  `, with args beginning with: `, even without arguments, and reports
  `SCAN`'s `NOVALUES` after another option as a syntax error.
- `HELLO` reports this server's name, version, and client id.
- `SELECT` accepts only database 0, the only one.
- `COMMAND`, `CONFIG`, `KEYS`, `FLUSHALL`, and most `CLIENT` subcommands are
  not implemented; tools such as `redis-benchmark` warn about `CONFIG` and carry
  on.
- `BGREWRITEAOF` with the AOF off replies with an error instead of writing a
  one-off file.

Anything else that differs from the latest Redis is a bug, unless the README
lists it as a deliberate difference.
