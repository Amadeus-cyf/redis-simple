# Server

Server behaviour in detail. Persistence has its own pages:
[`aof/README.md`](aof/README.md) and [`rdb/README.md`](rdb/README.md).

## Protected mode and authentication

Like Redis, the server starts in protected mode: when no password is set, it
only accepts clients connecting over loopback and answers others with a
`DENIED` error. Set a password with `--requirepass`, after which clients must
send `AUTH <password>` (or `AUTH default <password>`, or `HELLO <protover>
AUTH default <password>`) before other commands, or disable the check with
`--protected-mode no` on a trusted network.

## Client sockets

Client sockets are set up as Redis sets up its own: Nagle's algorithm is off
(`TCP_NODELAY`), so a reply is not held back for the acknowledgment of the
one before it, and the kernel probes a peer that has been silent for 300
seconds (Redis' default `tcp-keepalive`), so a client whose machine vanished
does not keep its connection forever. The interval is not configurable.

## Protocol

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

## Threaded I/O

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
   once for all the clients that ran in the same event-loop iteration.
3. Gets the client back, writes the replies, and resumes reading.

A client belongs to one side at a time, and mutex-guarded queues with pipe
wakeups carry it between them, so no thread spins waiting for another. Each
handoff moves its whole batch of clients under one lock acquisition and at
most one wakeup.
Closed connections are closed on their I/O thread and freed on the main
thread. `INFO stats` reports `io_threads_active`,
`io_threaded_reads_processed`, and `io_threaded_writes_processed`, and
`INFO threads` lists each I/O thread's clients, reads, and writes.

As with Redis, threads pay off only when the main thread is the bottleneck,
as with heavily pipelined traffic on a machine with spare cores; without
pipelining, round trips limit throughput and the default of 1 is as fast.

## Numbers

Numbers follow Redis 8: integer arguments must be canonical (no `+`, leading
zeros, or `-0`), scores and other floats parse like `strtod` (so `inf` and
hex work) while range bounds also take overflow, and scores print as Redis 8
prints them: integers up to 2^62 in full, otherwise the shortest digits in
`fpconv_dtoa`'s layout, such as `0.00001`, `1e-7`, or `6.948674738744653e+5`.

## SCAN and collection encodings

The `SCAN` family iterates as Redis 8 does. Each step of a hash-table scan
visits buckets until it has seen `COUNT` elements, or ten times `COUNT`
buckets, before filtering by `MATCH` or `TYPE`. A collection still in a
compact encoding (a listpack or intset) comes back whole in one step with
cursor 0. Collections use Redis 8's default encoding limits: hashes stay
listpacks up to 512 entries, sets and sorted sets up to 128 (sets of integers
stay intsets up to 512), and each element up to 64 bytes. So `HGETALL`, the
other whole-collection replies, and `*SCAN` return elements in the same
order as Redis while the collection is compact.

## Transactions

Transactions follow Redis semantics. After `MULTI`, commands are queued and
answered with `QUEUED`; `EXEC` runs them back to back, with no other client's
command in between, and returns their replies as one array. A command that
fails at runtime reports its error in that array without stopping the rest. A
command rejected while queueing (unknown name, wrong arity, or `NOAUTH`) makes
`EXEC` fail with `EXECABORT`. `WATCH` provides optimistic locking: if a watched
key is written, deleted, renamed, flushed, or expires before `EXEC`, `EXEC`
returns a null reply and runs nothing. `EXEC` and `DISCARD` unwatch every key,
and one transaction may queue at most 64 MiB of arguments.

## Freeing memory

`UNLINK` removes keys at once and frees a value of more than 64 elements
later, and `FLUSHDB`, in both modes, empties the database at once and frees
a keyspace of more than 4,096 keys later. Redis frees both on a background
thread; here the main thread frees them in steps of at most 10 ms per 100 ms
cron tick, so freeing millions of elements does not hold up other clients
and the memory goes back to the operating system (mimalloc only returns
memory that the thread which allocated it frees). Redis 8 replies to
`FLUSHDB SYNC` only once the database is freed; here the reply comes at
once. Command names, arity, access mode, and key positions are held in one
allocation-free registry used for case-insensitive dispatch and early argument
validation.

## Code layout

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
