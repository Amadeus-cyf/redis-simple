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

Always compare with the latest stable Redis release, not whichever version
is installed. `scripts/build_latest_redis.sh` finds the newest release tag,
builds it from source, and prints the directory holding its binaries; it
installs nothing, and CI's compatibility job uses it too:

```sh
PATH="$(scripts/build_latest_redis.sh build/reference-redis):${PATH}"
redis-server --version
```

With that directory first on `PATH`, every script below uses that server.
Set `REDIS_VERSION` to build a specific release instead.

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
file, then loads it in the other server and compares the keyspaces. It runs
each direction twice, with the base file as RDB (`aof-use-rdb-preamble yes`)
and as commands. Add new
write commands to its list. Its command list also makes a quick reply
comparison: extract it and send it to both servers in RESP2 and RESP3. Set
`INTEROP_KEEP=1` to keep the directories when it fails. A failure to load
usually means the other server wrote a command this one does not implement:
Redis' rewrite writes `SET`, `RPUSH`, `SADD`, `ZADD`, `HMSET`, and
`PEXPIREAT`.

## 4. Change counts and WATCH

Replies do not show everything a command does. After changing a write
command, compare its side effects too:

```sh
scripts/compare_side_effects.py build/debug/redis_simple
```

For each case it compares the replies, the change count (`INFO`'s
`rdb_changes_since_last_save`, which is what `Client::MarkModified` adds),
and whether a `WATCH` held by a second client is failed. The cases are
mostly commands that change nothing, such as `LTRIM` of a missing key or
`SADD` of an existing member, because that is where Redis' rules are easy to
miss: some signal the key without counting a change, and some do neither.
Add a case for every new write command and option.

## 5. RDB interoperability

After any change to `server/rdb/`, or to the byte format of `ListPack` or
`IntSet` (which RDB files store as they are), check that each server loads
the other's RDB file:

```sh
scripts/check_rdb_interop.py build/debug/redis_simple
```

It fills one server with every type in each encoding, with edge cases such
as entries whose encoded sizes sit at listpack backlen boundaries, saves,
starts the other server on the file, and compares every key: from this
server to Redis, from Redis with and without compression and checksum, and
back to itself. It needs Redis 7.2 or later, which reads the RDB version 11
this server writes. Add cases to `fill` when adding a type or encoding.
Sorted-set scores are compared by value: the file stores the double, and how
replies print it is `compare_with_redis.py`'s concern.
`--keep` keeps the files when it fails.

The comparison scripts start both servers with `--save ""`, so no background
save runs during a comparison. `BGSAVE`'s in-progress replies, save points,
and `MISCONF` depend on timing, so `redis_simple_integration_rdb` covers
them. To compare them by hand, start both servers with `--save "1 1"` in
directories of their own, delete the directories, write a key, and after two
seconds compare `SET`, `PING`, `INFO persistence`, and what `SIGTERM` does
(both keep running when the final save fails).

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
