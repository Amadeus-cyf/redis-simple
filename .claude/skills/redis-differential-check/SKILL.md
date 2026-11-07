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

## 2. Ad hoc comparisons

For behavior the script does not cover, send the same commands to both
servers and diff the replies. Cover the error paths and edge arguments, not
only the success case, and both protocols:

```sh
build/debug/redis_simple --port 8090 &
for port in 6379 8090; do
  redis-cli -p "${port}" <commands.txt >"replies.${port}.txt" 2>&1
  redis-cli -3 -p "${port}" <commands.txt >"replies3.${port}.txt" 2>&1
done
diff replies.6379.txt replies.8090.txt; diff replies3.6379.txt replies3.8090.txt
```

Use `FLUSHALL` between runs, and do not compare `SCAN`, `SMEMBERS`, or
`HGETALL` order, which depends on the hash seed. Repeat with
`--io-threads 3` after changing the request path.

## 3. AOF interoperability

After any AOF change, check that each server loads the other's AOF directory:

```sh
scripts/check_aof_interop.sh build/debug/redis_simple
```

It writes every data type, a TTL, and a transaction, with a rewrite in
between, so the directory holds a base file and an incremental file, then
loads it in the other server and compares the keyspaces. Set
`INTEROP_KEEP=1` to keep the directories when it fails. A failure to load
usually means the other server wrote a command this one does not implement:
Redis' rewrite writes `SET`, `RPUSH`, `SADD`, `ZADD`, `HMSET`, and
`PEXPIREAT`.

## Known differences

Not bugs; leave them unless the task is about them.

- Versions before 8: scores and other doubles print with `%.17g`, where
  Redis 8 (and this server) uses `d2string`'s shortest form; `SCAN` accepts
  invalid cursors; `ZRANK`/`ZREVRANK` have no `WITHSCORE` before 7.2.
- `HELLO` reports this server's name, version, and client id.
- `COMMAND`, `CONFIG`, `KEYS`, and most `CLIENT` subcommands are not
  implemented; tools such as `redis-benchmark` warn about `CONFIG` and carry
  on.
- `BGREWRITEAOF` with the AOF off replies with an error instead of writing a
  one-off file.

Anything else that differs from the latest Redis is a bug, unless the README
lists it as a deliberate difference.
