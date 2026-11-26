# Design

How redis-simple is put together, for readers changing it. The README
describes what the server does; `CLAUDE.md` lists the rules a change must
keep. Where a part follows a Redis function, its name is given so the two can
be compared.

## Layers

```text
containers/    listpack, quicklist, dict, skiplist, intset, handle set:
               containers that know nothing about Redis types
data_types/    set, list, sorted set, hash: Redis types built on those, each
               switching encodings as Redis does
server/db/     the keyspace (RedisDb), TTLs, WATCH, and RedisObject, which
               holds one value of any type
server/        clients, request parsing, replies, threaded I/O, the server
               lifecycle, the AOF (server/aof/), and RDB snapshots
               (server/rdb/)
server/commands/  the command table, and one file of handlers per command
               family (key.cpp, string.cpp, list.cpp, set.cpp, zset.cpp,
               hash.cpp, and the connection, transaction, persistence, and
               INFO commands)
```

Each layer uses only the layers listed before it, plus `utils/` and `logging/`.

## The keyspace

`RedisDb` keeps one hash table entry per key, a single allocation that holds
the key's name and its `RedisObject`, as Redis 8's kvobj does. The object is
the value's bytes (a collection, or a string of up to 31 bytes with libc++
and 39 with libstdc++, is stored in them) and one header word: a 3-bit tag
for the encoding and, in the other 61 bits, the key's expiration time. A
lookup that finds the key therefore has its TTL without another probe, and
a TTL adds nothing to the entry. With libc++ the entry is 56 bytes plus the
key's name, so a name of up to 8 bytes fits mimalloc's 64-byte size class
and one of up to 24 bytes the 80-byte class. Redis 8's entry is smaller for
a short key and a short value, because its object header is 16 bytes and
holds collections by pointer; here a collection of any size and a value of
up to 31 bytes need no second allocation. An expiration time too late for
61 bits, which only `PEXPIREAT` with a time millions of years away
produces, is kept exactly in a side table. Active expiration must still
sample only keys that have a TTL, so the database also keeps an index of
those entries (`containers::HandleSet`): an open-addressed array of 8-byte
entry handles, which costs a key with a TTL one slot and no allocation,
where Redis' expires dict costs it a pointer in a bucket. The index is
rebuilt a few slots per insert or delete when it fills up or empties, and
like `Dict` it does not shrink while a snapshot child shares its pages.

## A request's path

1. **Read.** The event loop reports a readable socket, and the client reads
   once into its query buffer (`Client::ReadQuery`). Reading once per event
   keeps one busy client from starving the others. The read fills all the
   room the buffer has, as in Redis, and an empty buffer keeps up to 64 KiB
   between requests, so after a request that fits the next one arrives in a
   single read instead of a first 16 KiB and the rest. Redis keeps a buffer
   of any size after a large argument, until the client is idle. This
   server frees a larger one as soon as it is empty, because mimalloc gives
   each allocation past 64 KiB a span of 64 KiB slices: a client that sent
   one 64 KiB value would hold 128 KiB, where Redis' jemalloc holds 80 KiB.
   A larger request is read as a first 16 KiB and then the rest.
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
6. **Flush, then reply.** The read handler does not reply. It leaves the
   client with the server (`Server::HoldReplies`), and when the event loop
   has run every client that was readable and is about to wait again,
   `Server::BeforeSleep` writes the AOF buffer to the file once
   (`Aof::Flush`) and only then sends the held replies, so a client never
   sees a reply for a write that is not in the file. This is Redis'
   `beforeSleep`, and it is a group commit: with `appendfsync always`, the
   clients served in one iteration share one `write` and one `fsync`
   instead of paying for one each. A write callback left from an earlier,
   partly sent reply also waits while its client is held. If the flush
   fails under `always`, the server stops and the held replies are never
   sent.

### Idle clients' buffers

A client's query buffer, reply buffer, and argument lists grow with its
largest request and reply. So that an idle connection does not keep them,
an `IdleClientSweeper` visits every client once a second, a tenth of them
per 100 ms tick, and a client that has read nothing since its previous
visit and holds no unparsed input or unsent reply frees them
(`Client::ReleaseIdleBuffers`); its next request allocates them again. This
is the job of Redis' `clientsCronResizeQueryBuffer` and
`clientsCronResizeOutputBuffer`. Without threaded I/O the server cron runs
the sweeper. With it, each I/O thread runs its own, from a timer on its
event loop, over the clients assigned to it, and skips clients the main
thread holds, as Redis' `IOThreadClientsCron` does.

### Returning freed memory

mimalloc gives each thread its own heap and keeps freed pages for reuse.
It returns a free page to the system only when the owning thread later
allocates or frees a page in the same segment, or collects its heap, and a
page another thread freed into the heap is not even known to be free until
then. Returning pages at every free instead (a purge delay of 0) cost a
system call and a page fault for every overwritten large value, and still
missed the pages other threads free. So the main thread and each I/O
thread tick a `MemoryPurger` from their 100 ms cron. Every tenth tick, if
any thread did work since this thread's last purge (events, freed client
buffers, expired keys, or a cron tick that freed detached values or moved a
rehash along, the last such tick included), it collects the thread's
heap, which returns its free pages. Work on any thread counts because the
threads free each other's memory: the I/O threads free reply buffers the
main thread allocated, and the main thread frees clients the I/O threads
read for. A shared atomic count of the ticks with work, which every
purger reads, tells each thread when to collect. Freed memory goes back
within about a second, busy or idle, for one collect per second per
thread while there is work (0.2 to 0.9 ms on heaps up to 3 GiB). Redis'
allocator similarly returns freed memory from a background thread after a
delay.

### What a command can reach

A handler gets its `Client`. Through it, it reaches its arguments, its reply
buffer, its connection state (protocol version, name, transaction, WATCHed
keys), and, through `Client::Server()`, the server-wide `ServerContext`: the
database, the AOF, the required password, and the I/O threads. The server
owns that context; tests and AOF replay build their own, so commands never
depend on a running `Server`.

## Key expiration

A key with a TTL is deleted when a command looks it up after it expired, and
otherwise by `ActiveExpire` (`server/active_expire.h`), which follows Redis'
`activeExpireCycle`. A round samples up to 20 keys from the TTL index,
going around it at most once, and deletes the expired ones; the cycle repeats
rounds while more than 10% of a round's sample was expired. The time budget
is checked every 16 rounds, so a cycle can run up to 16 rounds past it.

- The slow cycle runs from the server cron every 100 ms, for about 25 ms.
- The fast cycle runs from the event loop's before-sleep hook
  (`Loop::SetBeforeSleep`, called once per iteration right before the loop
  waits for events), for about 1 ms. It starts only if the previous cycle
  ran out of time or the running estimate of expired-but-present keys is
  at least 10%, and never within 2 ms of the previous fast cycle. When the slow
  cycle keeps up, the hook returns without reading a clock.

Differences from Redis that remain:

- A round has no cap on the slots it visits (Redis stops after 400
  buckets): the TTL index shrinks as it empties, so a round stays short.
- "Now" is read once per cycle; Redis reads it every round. A key that
  expires during a cycle is caught by the next one.
- A cycle does not stop when the scan has gone once around the TTL index.
- There is no `active-expire-effort` setting, and a sparsely filled TTL
  index is not skipped.

## Threaded I/O

With `--io-threads` above 1, I/O works as in Redis 8. Each client belongs to
the `IoThread` with the fewest clients when it connected. That thread's own
event loop reads the client's socket and frames its requests
(`Client::ParseQueries`), then hands the client to the main thread. The main
thread runs the requests (`Client::RunParsedQueries`) and keeps the clients
until `Server::BeforeSleep` has flushed the AOF, once for every client that
ran in that iteration, whichever I/O thread it came from. Then it hands the
clients back, and the I/O thread writes their replies and resumes reading.
Without an AOF there is nothing to flush, so each handoff's clients go back
as soon as their requests have run. Each direction moves a whole
batch at once: one lock acquisition, and one pipe wakeup only when the
receiving queue was empty, however many clients the batch holds, as Redis'
`sendPendingClientsToIOThreads` and `sendPendingClientsToMainThreadIfNeeded`
join whole lists.

So commands, the database, and the AOF stay on the main thread, and an I/O
thread only touches its clients' sockets, buffers, and parsers. A client is on
one side at a time. Before handing a client over, the I/O thread marks it
(`Client::HandedToMain`) and removes its write callback, but keeps the read
callback registered, as Redis 8 keeps its read handler, so a request costs no
`epoll_ctl` calls. If the socket becomes readable while the main thread holds
the client, the read callback unregisters itself, and the thread registers it
again when the client comes back. It hands clients over only after the
current loop iteration, since connection callbacks still use the connection
after they return. Connections close on their I/O thread; the main thread only frees the
`Client`.

## The append-only file

`server/aof/` follows Redis 8.12's `src/aof.c`. Its files:

| File | What it holds |
| --- | --- |
| `aof.h`, `aof.cpp` | `Aof`: appending, flushing, fsync policy, write errors, and manifest updates |
| `aof_load.cpp` | Startup: reading the manifest, upgrading a single-file AOF, replaying files, opening the file to append to |
| `aof_rewrite.cpp` | `BGREWRITEAOF` and automatic rewrites: the fork, installing the new base, retry backoff, and the cron |
| `manifest.h`, `.cpp` | The manifest format, and file naming |
| `records.h`, `.cpp` | Encoding mutations as AOF records |
| `snapshot.h`, `.cpp` | Writing the database as commands, for a base file with `--aof-use-rdb-preamble no` |
| `background_jobs.h`, `.cpp` | The background threads for file work: one syncs, one closes and deletes |
| `file_ops.h`, `.cpp` | File system helpers, and the `FileOps` interface tests use to inject failures |

### Files

The AOF is a directory with one base file (the snapshot the last rewrite
wrote), incremental files with the mutations since, and a manifest listing
them, in Redis' format. Loading replays the base, then each incremental file
in order. The base is an RDB snapshot (`<prefix>.<n>.base.rdb`, written by
`rdb::WriteRdbFile` with the `aof-base` aux field) unless
`--aof-use-rdb-preamble no` makes it commands (`.base.aof`). Like Redis,
`Aof::LoadRdbPreamble` decides by content, not name or option: a file starting
with `REDIS` loads through `rdb::LoadRdb` with `keep_expired` (Redis'
`RDBFLAGS_AOF_PREAMBLE`: commands after the snapshot were written while its
keys existed), and the replayer continues after the bytes the loader
consumed, counting offsets from the file's start so a torn tail is cut at the
right place. That also loads an old single-file AOF that begins with an RDB
preamble. A file the manifest lists that is missing is fatal, and only the
last file may end in a torn record, which is cut off. Restarts keep appending
to the last incremental file; only a rewrite starts a new one.

The manifest is replaced in an order that survives a crash at any point
(`PersistManifest`): write and sync a temporary file, rename it over the
manifest, then sync the directory. Files the new manifest retires are deleted
only after that, on the background file thread.

### Writing

`Aof::Append` only encodes a mutation into a buffer (Redis' `aof_buf`).
Commands whose effect depends on the clock are recorded by their result: a
relative TTL becomes an absolute `PEXPIREAT`, and a `SET` with options
becomes the value and TTL it left (a `SET` without options is recorded as
given, since it has only one outcome). An `EXEC`'s records are wrapped
in `MULTI`/`EXEC` records when there is more than one, so replay applies all
or none of them.

`Aof::Flush` (Redis' `flushAppendOnlyFile`) writes the buffer. With
`everysec` it queues an fsync on the background sync thread once a second, and
when a sync is still running it postpones the write for up to two seconds,
since on Linux the write would block behind it. With `always` it syncs on the
event-loop thread. After a write, a buffer that grew past 1 MiB is released,
so one huge command does not keep its size in memory.

A failed write cuts any partial write back off the file and keeps the buffer
to retry, and write commands and `PING` get `-MISCONF` until a write succeeds
(`Client::WritesRefused`, which checks a failed background save first, as
Redis' `writeCommandsDeniedByDiskError` does). A failed
background fsync rejects writes until the next one succeeds. Only with
`always` does a failure stop the server, since replies already promised the
data was on disk.

### Rewriting

A rewrite first switches appends to a new incremental file, so writes made
during the rewrite need no buffering. It then forks, unless a background
save's child is running (see Background saves). The child writes a
snapshot of its copy-on-write view of the database (`Aof::WriteBaseFile`: an
RDB file, or commands through `WriteSnapshotFile`) and exits; its exit code reports how it went. The server cron notices the exit
(`Aof::Cron`), renames the snapshot to the next base file, and persists a
manifest listing it and the new incremental file. The old base and incremental
files become history and are deleted once that manifest is durable.

`fork` copies only the calling thread, so if another thread held a lock (such
as the allocator's) at that moment, the child would inherit it locked
forever. The server's fork hooks pause the I/O threads. (Values `UNLINK` and
`FLUSHDB` detach are freed on the main thread, in steps from the cron, not
on a thread of their own.) The background job threads never
allocate, free, or log, so they can keep running: their jobs hold paths in
fixed-size buffers, their queues keep their memory when cleared, and the
thread that queues a sync-then-close reserves the room the sync thread needs
to hand the close over.

`BackgroundJobs` runs two threads, each with its own queue, as Redis' `bio.c`
runs one per job class: the sync thread only syncs files (`BIO_AOF_FSYNC`),
and the file thread closes and deletes them (`BIO_CLOSE_FILE`). Closing the
last descriptor of a deleted file, or deleting a large one, frees its blocks
and can take seconds; on a thread of its own it cannot hold up the
every-second sync, which would otherwise make `Aof::Flush` postpone writes.
A file to sync and then close is synced on the sync thread and closed on the
file thread. `Rdb` has its own `BackgroundJobs` for the temporary file of a
background save that was stopped. The child
itself takes no locks and does not log.

Hash tables hold back while a child lives. Moving entries to a bigger table
writes to memory the child still shares, and the kernel must copy every page
touched. So the fork hooks set a process-wide resize policy
(`containers::SetDictResizePolicy`), as Redis' `updateDictResizePolicy`
does: forbidden in the child, which never moves an entry, and "avoid" in the
parent until the cron sees no child. Under "avoid" a `Dict` grows only once
it holds four entries per bucket, shrinks only when far emptier than usual,
and continues a rehash already under way only when the two tables differ
greatly in size. Growing the 4-million-key keyspace table during a `BGSAVE`
copied about 280 MB before this policy and about 34 MB with it.

Commands move a rehash along, one bucket per operation on the table, so on
their own an idle server would keep both the old and the new table. The
server cron therefore calls `RedisDb::RehashIncrementally` every tick while
no child runs, as Redis' `databasesCron` does: it moves buckets of the
keyspace table and slots of the TTL index for one millisecond
(`Dict::RehashSome`, `HandleSet::RebuildSome`), and first retries a shrink
(`ShrinkIfNeeded`) that deletes could not start while a child ran.

Snapshots are bounded: large strings are split into a `SET` and `APPEND`s,
and collections are batched by encoded bytes as well as element count, so
every command fits the replay limit (`Limits`).

### Deliberate differences from Redis

- When a manifest was renamed into place but the directory sync failed, the
  manifest is live but may not survive a crash. Redis treats it as a failure
  and deletes the files it lists, where it can lose data; this server adopts
  it, rejects writes, and keeps the replaced files until a later manifest
  persists safely.
- `INFO persistence` has no `aof_cmd_duration`, Redis 8.12's estimate of the
  AOF's replay time, which Redis adds up from each command's measured run
  time: this server does not time commands.

Until Redis 8.12, a rewrite that started while a failed write was pending was
a third difference: Redis carried the unwritten bytes into the new
incremental file, where replay applied them twice. Redis 8.12 refuses that
rewrite and counts it as a failure, as this server does.

## RDB snapshots

`server/rdb/` follows Redis 8.12's `src/rdb.c`. Its files:

| File | What it holds |
| --- | --- |
| `rdb.h`, `rdb.cpp` | `Rdb`: `SAVE` (temporary file, rename, directory sync), `BGSAVE`'s child, save points, the save at shutdown, loading at startup, `LASTSAVE` and the `INFO` fields |
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
and skiplists as counted elements. A skiplist sorted set is written from its
greatest member down, as Redis' `rdbSaveObject` does, so that a loader
inserting in file order always inserts at the head of its skiplist, without
searching past smaller members. Blobs are copied as they are, which works
because `Listpack` and `Intset` keep Redis' byte format in memory: little-endian
headers, lengths, and integers, and Redis' backlen layout (unit tests check
the bytes against vectors Redis produced). Strings are encoded as Redis'
`rdbSaveRawString` does: short decimal text as an integer, longer text
LZF-compressed when that saves at least four bytes.

The writer buffers records and writes them in 1 MiB blocks, but writes a
large value straight from the object, so it is never copied. It takes no
locks and does not log, so a forked child can run it.

Both snapshot writers (this one and the AOF rewrite's `WriteSnapshotFile`)
push every 4 MiB written towards the disk through `aof::IncrementalSync`, as
the automatic sync of Redis' `rioFileWrite` does, so dirty pages never pile
up for the final `fsync`. On Linux each interval after the first calls `sync_file_range` twice:
once to start writing its own bytes without waiting, and once to wait for
the interval before it, which has had a whole interval to reach the disk. A
full `fsync` per interval would also flush the file's metadata and block the
writer for all of it. Other platforms have no such call and `fsync` each
interval. After each interval, and after the final `fsync` (which every
platform still does before the rename), the writer asks the kernel to drop
the file's pages from its page cache (`posix_fadvise` with `DONTNEED`, on
Linux), so a snapshot does not crowd out the dataset's memory. As in Redis,
the save at shutdown keeps them (`RDBFLAGS_KEEP_CACHE`): the next start
reads that file. These calls go through `aof::FileOps`, where unit tests
record their order.

The file declares RDB version 11, the oldest that covers every type written,
so Redis 7.2 and later load it.

### Loading

The loader reads the file through a buffer that tracks the CRC64 and the bytes
left. Every length the file claims is checked against those bytes before
anything is allocated, an LZF string's expanded size against the 512 MiB
string limit, and listpacks and intsets are validated as they are read
(`containers::ListpackReader` checks each entry's encoding, size, and backlen, as
Redis' deep validation does). A listpack's backlens are widened first where
Redis 8.8 to 8.10, or an earlier build of this server, wrote them a byte
shorter than other releases (`containers::WidenCompactBacklens`, Redis 8.12's
`lpUpgradeCompactBacklen`); unlike Redis, which does so only in RDB versions 14
and 15, this server does it in every version, since its own files are
version 11. As Redis does, a set, hash, or sorted set
listpack is then kept as stored, so loading it is one copy: one pass over it
checks every element, that keys are distinct (by sorting their views), and
that scores are numbers, and it is kept only if it also fits this server's
listpack limits, holds sorted set pairs in (score, member) order, and stores
each element as `Listpack` would, an integer exactly when its text parses as
one, which lookups rely on. Any other value, including lists and intsets, is
rebuilt through the data types, so no corrupt structure enters memory. A
hash, set, or sorted set stored as a hash table and counting more entries
than a listpack holds starts in that encoding, sized for the count, instead of
filling a listpack and converting it. The keyspace and expiration tables are
sized once from the `RESIZEDB` counts (Redis' `dbExpand`), so they do not
rehash while the keys arrive; each count is first cut down to the keys the
bytes left in the file could hold (three bytes a key at the least), so a
false count cannot make the server allocate more than a few times the file's
size. Duplicate members, fields, or keys, NaN
scores, and types this server does not have stop the load with a message
naming the offset; empty collections are skipped, and keys whose expiration
time has passed are dropped, as Redis does.

### Background saves

`BGSAVE` forks a child that writes `temp-<child pid>.rdb`, renames it over the
file, and syncs the directory, as `SAVE` does, then exits with a code that
says which step failed; like the AOF rewrite's child it takes no locks and
does not log, and the same fork hooks pause the threads that allocate. The
server cron (`Rdb::Cron`) reaps it. On success the changes it covered are
subtracted from the change counter (Redis' `server.dirty`), since writes made
while it ran are not in the file, and `LASTSAVE` moves to when it ended. A
child killed by a signal leaves its temporary file, which the parent deletes
on its background file thread (as Redis' `bg_unlink` does, since the file can
hold most of a snapshot);
`SIGUSR1`, which shutdown uses to stop it, is not counted as a failure.

As in Redis, only one child runs at a time. The cron passes each side whether
the other has a child (`Aof::Cron` and `Rdb::Cron` take `may_fork`), so a
scheduled or automatic rewrite waits for a save and a scheduled save or save
point waits for a rewrite. `BGSAVE` during a rewrite fails unless it is
`BGSAVE SCHEDULE`, `BGREWRITEAOF` during a save is scheduled, and inside
`EXEC` both are scheduled for the cron.

Commands count their changes through `Client::MarkModified(changes)` as
Redis' commands add to `server.dirty`: one per key set or deleted, member or
field pair given, or element pushed, popped, or trimmed. Each client holds a
pointer to the RDB's counter (`ServerContext::changes`), so counting is one
addition; AOF replay counts into its own client, and the total is added at
startup. A save point is reached when there are at least its changes and
more than its seconds have passed since the last save; after a failure the
cron waits five seconds between tries (Redis' `CONFIG_BGSAVE_RETRY_DELAY`).

A failed background save sets `Rdb::DeniesWrites` when there are save points
and `--stop-writes-on-bgsave-error` is on. `Client::ValidateRequest` then
refuses commands the table marks `refused_on_disk_error` (write commands and
`PING`) with Redis' `MISCONF` text, and `EXEC` with queued writes discards the
transaction with `-EXECABORT`, as Redis does. Any successful save clears it.

On `SIGTERM` or `SIGINT`, the cron calls `Rdb::SaveBeforeShutdown` before
stopping the loop: it kills a running child and waits for it, so the child
cannot replace the file, then saves when there are save points. If that save
fails the server logs Redis' message and keeps running.

