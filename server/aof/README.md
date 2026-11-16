# Append-only file

How the server writes, rewrites, and loads its append-only file (AOF).
`docs/design.md` covers the code structure; the server options are listed
by `redis_simple --help`.

## Enabling

Enable AOF persistence with Redis-style fsync policies:

```sh
./build/debug/redis_simple --appendonly yes \
  --appenddirname appendonlydir --appendfilename appendonly.aof \
  --appendfsync everysec \
  --auto-aof-rewrite-min-size 67108864 \
  --auto-aof-rewrite-percentage 100
```

## Files

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

## What is recorded

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

## Writing and syncing

Mutations are encoded into a buffer that is written before any of their replies
are sent, as in Redis. `everysec`, the default, syncs on a background thread
once a second; as in Redis, a write waits up to two seconds while that sync
still runs (`aof_delayed_fsync` counts the times it stopped waiting), so an
operating-system crash can cost about the last two seconds. `always` syncs each
batch (group commit), and `no` leaves syncing to the operating system. Graceful
shutdown writes and syncs everything pending. `--no-appendfsync-on-rewrite yes`
skips syncs while a rewrite or a background save runs, and `--aof-timestamp-enabled yes` writes
Redis' `#TS:` annotations, which loading skips.

## Rewriting

`BGREWRITEAOF` compacts command history. It first switches appends to a new
incremental file and lists it in the manifest, then forks a child that writes
a snapshot of the database, from its copy-on-write view of memory, as
`temp-rewriteaof-bg-<pid>.aof`, pushing every 4 MiB written to the disk
(`--aof-rewrite-incremental-fsync`; on Linux with `sync_file_range`, as
Redis does, elsewhere with `fsync`); the server keeps serving meanwhile. In
the command format, large strings are split into replay-safe commands, and
collection batches are limited by encoded bytes. When the child finishes, its file becomes the next
base file, and a new manifest lists it with the incremental file opened at the
start. The old base and incremental files are then deleted on a background
thread, as Redis' bio threads do; syncs run on a separate thread, so a slow
close or deletion does not delay them. Writes made during the rewrite are already
in that incremental file, so nothing is buffered or copied at the end. Inside
`MULTI`, `BGREWRITEAOF` is scheduled and the cron starts it after `EXEC`.
Shutdown kills a rewrite that has not been installed yet; the previous files
stay listed.

## Loading

Replay loads the base, then each incremental file in order; a listed file that
is missing stops startup. Only the last file may end in a torn record, which is
cut off unless `--aof-load-truncated no`. `--aof-load-corrupt-tail-max-size`
(default 0, off) also cuts off a format error with fewer than that many bytes
after the last complete command.

## Automatic rewrites

Automatic rewriting starts once the AOF is larger than 64 MiB and has grown by
100% from its size after the last rewrite (or its base file's at startup). The
two `--auto-aof-rewrite-*` options override those defaults; a percentage of `0`
disables automatic rewriting. After three consecutive failed rewrites,
automatic and scheduled rewrites wait one minute, then two, four, and so on up
to an hour, as Redis does; `BGREWRITEAOF` still starts one at once. `INFO
persistence` reports Redis' AOF fields.

## Write errors

When the file cannot be written, as on a full disk, the server cuts a partial
write back off the file, keeps the buffer, and retries it; until a write
succeeds, it rejects write commands, and `PING`, with Redis' `-MISCONF Errors
writing to the AOF file` error while reads keep working; `EXEC` with queued
writes discards the transaction with `-EXECABORT`. A failed background fsync rejects
writes the same way until the next fsync succeeds. With `always`, which
promises that every acknowledged write is synced, a failed write or sync stops
the server instead.

## Differences from Redis

Two failure cases differ from Redis on purpose, where Redis can lose data. When
a new manifest is renamed into place but syncing the directory fails, Redis
deletes the new files the live manifest lists; this server keeps them, rejects
writes, and keeps the replaced files until a later manifest persists safely.
And a rewrite does not start while a failed write is still pending, since
Redis would then replay those commands twice.
