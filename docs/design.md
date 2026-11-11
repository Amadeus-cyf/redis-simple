# Design

How redis-simple is put together, for readers changing it. The README
describes what the server does; `CLAUDE.md` lists the rules a change must
keep. Where a part follows a Redis function, its name is given so the two can
be compared.

## Layers

```text
memory/        listpack, quicklist, dict, skiplist, intset: containers that
               know nothing about Redis types
data_types/    set, list, sorted set, hash: Redis types built on those, each
               switching encodings as Redis does
server/db/     the keyspace (RedisDb), TTLs, WATCH, and RedisObject, which
               holds one value of any type
server/        clients, request parsing, replies, threaded I/O, the server
               lifecycle, the AOF (server/aof/), and RDB snapshots
               (server/rdb/)
server/commands/  one handler per command, and the command table
```

Each layer uses only the layers listed before it, plus `utils/` and `logging/`.

## A request's path

1. **Read.** The event loop reports a readable socket, and the client reads
   once into its query buffer (`Client::ReadQuery`). Reading once per event
   keeps one busy client from starving the others.
2. **Frame.** `request_parser::Parse` finds one complete request in the
   buffer without copying it: the command name and arguments are
   `std::string_view`s into the buffer (`CommandArgs`). It frames RESP arrays
   and inline commands as Redis' `processMultibulkBuffer` and
   `processInlineBuffer` do, including their limits and error messages. Only
   quoted inline arguments, which need unescaping, are copied, into the
   client's `ParseState`.
3. **Validate.** `Client::ValidateRequest` looks the command up in the table
   (`commands::Find`), then checks arity, authentication, and whether the AOF
   accepts writes.
4. **Run.** The handler reads its arguments, works on the database, and
   appends its reply to the client's output buffer. Inside `MULTI` the
   command is copied into the client's `MultiState` instead and runs at
   `EXEC`.
5. **Propagate.** A handler that changed data calls `MarkModified()`. The
   client then touches WATCHed keys and appends the command to the AOF
   buffer (`Aof::Append`).
6. **Flush, then reply.** After the batch of requests read from the socket
   has run, the read handler writes the AOF buffer to the file
   (`Aof::Flush`), and only then sends the replies, so a client never sees a
   reply for a write that is not in the file.

### What a command can reach

A handler gets its `Client`. Through it, it reaches its arguments, its reply
buffer, its connection state (protocol version, name, transaction, WATCHed
keys), and, through `Client::Server()`, the server-wide `ServerContext`: the
database, the AOF, the required password, and the I/O threads. The server
owns that context; tests and AOF replay build their own, so commands never
depend on a running `Server`.

## Threaded I/O

With `--io-threads` above 1, I/O works as in Redis 8. Each client belongs to
the `IoThread` with the fewest clients when it connected. That thread's own
event loop reads the client's socket and frames its requests
(`Client::ParseQueries`), then hands the client to the main thread. The main
thread runs the requests (`Client::RunParsedQueries`), flushes the AOF once
for every client in the handoff, and hands the clients back, and the I/O
thread writes their replies and resumes reading.

So commands, the database, and the AOF stay on the main thread, and an I/O
thread only touches its clients' sockets, buffers, and parsers. A client is on
one side at a time: the I/O thread unregisters its socket events before
handing it over, and hands clients over only after the current loop
iteration, since connection callbacks still use the connection after they
return. Connections close on their I/O thread; the main thread only frees the
`Client`.

## The append-only file

`server/aof/` follows Redis 8.10's `src/aof.c`. Its files:

| File | What it holds |
| --- | --- |
| `aof.h`, `aof.cpp` | `Aof`: appending, flushing, fsync policy, write errors, and manifest updates |
| `aof_load.cpp` | Startup: reading the manifest, upgrading a single-file AOF, replaying files, opening the file to append to |
| `aof_rewrite.cpp` | `BGREWRITEAOF` and automatic rewrites: the fork, installing the new base, retry backoff, and the cron |
| `manifest.h`, `.cpp` | The manifest format, and file naming |
| `records.h`, `.cpp` | Encoding mutations as AOF records |
| `snapshot.h`, `.cpp` | Writing the database as commands, for a new base file |
| `background_jobs.h`, `.cpp` | The background thread that syncs, closes, and deletes files |
| `file_ops.h`, `.cpp` | File system helpers, and the `FileOps` interface tests use to inject failures |

### Files

The AOF is a directory with one base file (the snapshot the last rewrite
wrote), incremental files with the mutations since, and a manifest listing
them, in Redis' format. Loading replays the base, then each incremental file
in order. A file the manifest lists that is missing is fatal, and only the
last file may end in a torn record, which is cut off. Restarts keep appending
to the last incremental file; only a rewrite starts a new one.

The manifest is replaced in an order that survives a crash at any point
(`PersistManifest`): write and sync a temporary file, rename it over the
manifest, then sync the directory. Files the new manifest retires are deleted
only after that, on the background thread.

### Writing

`Aof::Append` only encodes a mutation into a buffer (Redis' `aof_buf`).
Commands whose effect depends on the clock are recorded by their result: a
relative TTL becomes an absolute `PEXPIREAT`. An `EXEC`'s records are wrapped
in `MULTI`/`EXEC` records when there is more than one, so replay applies all
or none of them.

`Aof::Flush` (Redis' `flushAppendOnlyFile`) writes the buffer. With
`everysec` it queues an fsync on the background thread once a second, and
when a sync is still running it postpones the write for up to two seconds,
since on Linux the write would block behind it. With `always` it syncs on the
event-loop thread.

A failed write cuts any partial write back off the file and keeps the buffer
to retry, and write commands get `-MISCONF` until a write succeeds. A failed
background fsync rejects writes until the next one succeeds. Only with
`always` does a failure stop the server, since replies already promised the
data was on disk.

### Rewriting

A rewrite first switches appends to a new incremental file, so writes made
during the rewrite need no buffering. It then forks. The child writes a
snapshot of its copy-on-write view of the database (`WriteSnapshotFile`) and
exits; its exit code reports how it went. The server cron notices the exit
(`Aof::Cron`), renames the snapshot to the next base file, and persists a
manifest listing it and the new incremental file. The old base and incremental
files become history and are deleted once that manifest is durable.

`fork` copies only the calling thread, so if another thread held a lock (such
as the allocator's) at that moment, the child would inherit it locked
forever. The server's fork hooks pause the I/O threads and the database's
thread that frees `UNLINK`ed values. The background job thread never
allocates, frees, or logs, so it can keep running: its jobs hold paths in
fixed-size buffers, and its queues keep their memory when cleared. The child
itself takes no locks and does not log.

Snapshots are bounded: large strings are split into a `SET` and `APPEND`s,
and collections are batched by encoded bytes as well as element count, so
every command fits the replay limit (`Limits`).

### Deliberate differences from Redis

Two, both where Redis can lose data:

- When a manifest was renamed into place but the directory sync failed, the
  manifest is live but may not survive a crash. Redis treats it as a failure
  and deletes the files it lists; this server adopts it, rejects writes, and
  keeps the replaced files until a later manifest persists safely.
- A rewrite does not start while a failed write is pending, since Redis would
  carry the unwritten bytes into the new incremental file, where replay
  applies them twice.

## RDB snapshots

`server/rdb/` follows Redis 8.10's `src/rdb.c`. Its files:

| File | What it holds |
| --- | --- |
| `rdb.h`, `rdb.cpp` | `Rdb`: `SAVE` (temporary file, rename, directory sync), loading at startup, `LASTSAVE` |
| `rdb_writer.h`, `.cpp` | Writing a database in the RDB format (Redis' `rdbSaveRio`) |
| `rdb_loader.h`, `.cpp` | Reading one into a database (Redis' `rdbLoadRio` and `rdbLoadObject`) |
| `rdb_format.h` | The format's opcodes, value types, and encodings (Redis' `rdb.h`) |

`utils/crc64` (Redis' CRC-64/Jones) checksums the file, and `utils/lzf`
compresses strings.

### Writing

The file is Redis': the `REDIS` magic and version, aux fields, then, for
database 0, `SELECTDB` and `RESIZEDB` and each key with its expiration time,
then `EOF` and the CRC64 of everything before it. Each value is written in
the encoding Redis writes for the same in-memory encoding: an intset or a
listpack as one blob, a quicklist as its nodes' listpacks, and hash tables
and skiplists as counted elements. Blobs are copied as they are, which works
because `ListPack` and `IntSet` keep Redis' byte format in memory: little-endian
headers, lengths, and integers, and Redis' backlen layout (unit tests check
the bytes against vectors Redis produced). Strings are encoded as Redis'
`rdbSaveRawString` does: short decimal text as an integer, longer text
LZF-compressed when that saves at least four bytes.

The writer buffers records and writes them in 1 MiB blocks, but writes a
large value straight from the object, so it is never copied. It syncs every
4 MiB, and it takes no locks and does not log, so a forked child can run it.
The file declares RDB version 11, the oldest that covers every type written,
so Redis 7.2 and later load it.

### Loading

The loader reads the file through a buffer that tracks the CRC64 and the bytes
left. Every length the file claims is checked against those bytes before
anything is allocated, an LZF string's expanded size against the 512 MiB
string limit, and listpacks and intsets are validated as they are read
(`memory::ListPackReader` checks each entry's encoding, size, and backlen, as
Redis' deep validation does). Values are not adopted as stored: they are
rebuilt through the data types, so they take this server's encodings and no
corrupt structure enters memory. Duplicate members, fields, or keys, NaN
scores, and types this server does not have stop the load with a message
naming the offset; empty collections are skipped, and keys whose expiration
time has passed are dropped, as Redis does.

