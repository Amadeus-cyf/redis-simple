---
name: track-redis-release
description: Bring this server in line with a new Redis release. Use when CI's Redis Compatibility job starts failing without a code change here, or when asked to move to a newer Redis version.
---

# Track a new Redis release

CI's compatibility job builds the latest stable Redis release on every run
(`scripts/build_latest_redis.sh`), so a new release can turn it red on an
unrelated pull request. Fix that in a pull request of its own.

## 1. Reproduce

```sh
scripts/build_latest_redis.sh --print-version
PATH="$(scripts/build_latest_redis.sh build/reference-redis):${PATH}"
cmake --preset debug && cmake --build --preset debug --target redis_simple
```

Then run every script in the `redis-differential-check` skill and collect
the differences. To confirm the release caused them, build the previous one
(`REDIS_VERSION=<previous> scripts/build_latest_redis.sh
build/previous-redis`) and check that it reports none.

## 2. Decide each difference

Read the release notes and the changed code in Redis (`src/t_*.c`,
`src/db.c`, `src/server.c`, `src/aof.c`, `src/rdb.c`) for each one.

- Changed behavior of a command this server has: match it, and add the case
  to `scripts/compare_with_redis.py` or `scripts/compare_side_effects.py` if
  no case caught it.
- A new option on a supported command: implement it, following the
  `add-command` skill.
- A new command or type: out of scope unless asked. The scripts send only
  supported commands, so this alone does not fail them.
- A difference this server keeps on purpose: list it under "Known
  differences" in the `redis-differential-check` skill and in the README.

## 3. File formats

- RDB: if the release writes a newer RDB version, raise the newest version
  the loader accepts in `server/rdb/rdb_format.h`, and load a file the new
  release wrote, as `server/rdb/testdata/` and `RdbTest` do for the current
  one. Keep writing the oldest version that covers what is written.
- Listpack and intset bytes: the unit tests compare with bytes Redis
  produced. Regenerate a vector only if the release changed the encoding.
- AOF: check the manifest format and `INFO persistence` fields against the
  release's `src/aof.c`.

## 4. Record the version

Update the release named in `CLAUDE.md`, `README.md`, `docs/design.md`, and
the comments that say which release a behavior or byte vector follows
(`grep -rn "Redis 8\."`). Then run the `pre-pr-check` skill.
