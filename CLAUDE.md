# CLAUDE.md

Guidance for AI coding agents working in this repository.

## Project Style

- Keep this a modern C++23 project. `.clang-format` stays at `c++20`, the
  newest value clang-format accepts; the formatting rules are the same.
- Follow Google C++ style and the checked-in `.clang-format`. `.clang-tidy`
  enables the `google-*` checks and Google naming: `kName` for constexpr,
  `const` static, and global constants (function-local `static const`
  included), and sized integer types (`int64_t`, `uint32_t`) instead of
  `long` or `unsigned int`, except where a system API defines the type.
- Prefer clear, simple modern C++ over legacy patterns. Use the standard
  library where it fits (`std::span`, `std::source_location`,
  `starts_with`/`ends_with`, `<bit>`, concepts, `std::ranges` algorithms
  with projections, `std::cmp_*` for mixed-sign comparisons), but avoid
  `std::format` and `std::print`, which need newer standard libraries than
  the minimum compilers in `README.md`. CI runs clang-tidy 22, which
  checks more of these than older local versions do.
- Keep naming consistent with nearby code. Name things with whole words, as
  Google style asks: `index`, `length`, `value`, `buffer`, `connection`, not
  `idx`, `len`, `val`, `buf`, `conn`. Universally known short names (`i`, `n`,
  `fd`, `it`) and Redis or POSIX terms (`backlen`, `iov_len`) are fine. In
  listpack code, `offset` is a byte position in the buffer, and `index` or
  `position` is an element's place in the list.
- Use `#define` header guards named `REDIS_SIMPLE_<PATH>_<FILE>_H_`, not
  `#pragma once`.
- Do not use exceptions. Report recoverable failures through return values
  (`bool`, `std::optional`, status enums, or `nullptr`), and guard
  programming errors with `RS_CHECK` / `utils::Unreachable()` from
  `utils/check.h`, which abort and stay enabled in release builds. Test them
  with `EXPECT_DEATH` in suites named `<Suite>DeathTest`, which the macOS leak
  check skips. The only `catch` blocks are where `std::thread` reports a
  failure to start, and the catch-all in each integration test's `main`,
  which turns an escaping standard-library exception into a failing exit
  code.
- Spell project includes from the repository root (`"server/db/db.h"`), and
  include GoogleTest as `<gtest/gtest.h>`.
- Keep objects with static storage duration trivially destructible. When one
  must hold a non-trivial type, leak it (`static T& t = *new T;`).
- Use `Create()` for owning factory functions and return `std::unique_ptr`
  instead of owning raw pointers.
- Prefer concise accessor names such as `Type()`, `Encoding()`, and
  `TotalBytes()` over Java-style `Get...` names for new or renamed APIs.
- Prefer `std::string_view` for read-only string inputs and visitor-style
  traversal for hot paths; keep vector-returning helpers for convenience APIs
  and tests, not command execution paths that can stream replies directly.
- Command handlers should read arguments through `CommandArgs`
  (`std::string_view`s into the client query buffer). Copy arguments only at an
  ownership boundary, such as storing a key/value in the DB or data structure.
- Keep request decoding incremental and zero-copy. Standard RESP arrays are the
  primary wire format; inline commands remain a compatibility path. Frame both
  as Redis' `processMultibulkBuffer` and `processInlineBuffer` do, with its
  limits and `Protocol error` messages. Inline requests and AOF manifest
  lines split with `utils::SplitArgs` (Redis' `sdssplitargs`); only quoted
  inline arguments are copied, into the client's `ParseState`, which
  releases them once the requests have run. Keep the parser's hot path
  inlined: inner helpers return small structs by value (status, error, and
  values together) instead of filling out-parameters, so their fields stay in
  registers; length lines of one or two digits skip the digit loop; and rare
  cases (slow length lines, inline requests, resuming a split request) stay
  in out-of-line functions.
- Parse numbers as Redis does: integer arguments with `utils::ToInt64`
  (`string2ll`), floats with `utils::ToDouble` (`string2d`), and score range
  bounds with `utils::ToRangeDouble` (`zslParseRange`). Format doubles with
  `utils::FormatFloat`, which prints them as Redis 8's `d2string` does.
- Keep protocol-sensitive replies based on the client's negotiated RESP
  version. Connections default to RESP2 and may switch through `HELLO`.
- Pass `CommandArgs` by const reference instead of copying its view vector.
  Move completed `std::string` replies into `Client::AddReply`; use the
  `std::string_view` overload only for borrowed reply data. Send a lone
  integer or score with `Client::AddIntegerReply` or `AddFloatReply`, which
  encode on the stack, and call `reply::ReserveBulkStrings` before encoding
  a collection whose element count, or a bound on it, is known.
- Add comments only when they clarify non-obvious behavior. When a comment
  names the Redis function or field the code follows, also say in plain
  words what it does, for readers who do not know Redis.
- Keep command handler declarations grouped in `server/commands/handlers.h`;
  avoid per-command headers unless a handler becomes a broader shared API.
  Implement handlers in one file per command family,
  `server/commands/<family>.cpp`, in the namespace of the same name
  (`commands::key`, `string`, `list`, `set`, `zset`, `hash`, `connection`,
  `transaction`, `persistence`, `info`); do not split a family into a file
  per command.
- Name directories and namespaces for what they hold, and keep the two the
  same: containers that know nothing about Redis live in `containers/`
  (`containers::`), and the Redis types built on them in `data_types/`
  (`data_types::Set`, `List`, `Hash`, `ZSet`). Files under `utils/` are named
  for their subject without repeating the directory (`utils/strings.h`,
  `utils/floats.h`, `utils/integers.h`, `utils/time.h`).
- Keep command names, arity, access mode, and key positions in the sorted
  metadata table in `server/commands/command.cpp`. Preserve its allocation-free,
  case-insensitive lookup path: `Find` hashes the name with branch-free case
  folding and probes a compile-time hash index built from the table, whose
  names must be upper-case letters only and at most 16 bytes.
- When adding or changing supported commands, update the README command
  coverage list and the relevant command-family integration test.
- Keep reply encoding helpers directly under `server/`, database state and
  Redis object wrappers under `server/db/`, the AOF under `server/aof/`, and
  RDB snapshots under `server/rdb/`.
- Keep production and command integration servers on the shared `Server::Run`
  lifecycle. Signal handlers may only update signal-safe state; cleanup belongs
  on the event-loop thread.
- `docs/design.md` explains how requests flow, threaded I/O, the AOF, and RDB
  snapshots.
  Read the relevant part before changing them, and update it with them.
- The AOF lives in `server/aof/` (see its file table in `docs/design.md`).
  Keep its logic aligned with the latest Redis release's `src/aof.c` (8.10 at
  the time of writing): manifest format and parsing, file naming and sequence
  numbers, startup, upgrade, loading, flushing, fsync errors, rewrite
  scheduling and limiting, and `INFO` fields. Document any deliberate
  difference in `server/aof/README.md` and `docs/design.md`; two exist, where
  Redis can lose data. Check interoperability with a `redis-server` when one is
  available: each must load the other's AOF directory, with the base file
  as RDB (`aof-use-rdb-preamble yes`, the default) and as commands. Detect a
  base's format by its content (`REDIS`), as Redis does, never by its name.
- RDB snapshots live in `server/rdb/` (see `docs/design.md`). Keep the file
  format and `SAVE` aligned with the latest Redis release's `src/rdb.c`:
  values in the RDB types Redis writes for each in-memory encoding, strings
  through `rdbSaveRawString`'s integer and LZF rules, the CRC64, and the
  temporary-file, rename, and directory-sync order. Write RDB version 11, the
  oldest that covers what is written, unless a new type needs more. Check
  interoperability with `scripts/check_rdb_interop.py`. Keep `BGSAVE`, save
  points, the save at shutdown, `stop-writes-on-bgsave-error`, and the `rdb_`
  `INFO` fields aligned with it too.
- Both snapshot writers (the RDB writer and the AOF's command snapshot)
  sync as they go through `aof::IncrementalSync`, which follows Redis'
  `rioFileWrite`: on Linux `sync_file_range` starts each 4 MiB on its way
  and waits only for the 4 MiB before it, elsewhere it is an `fsync`, and
  on Linux the file's written pages are dropped from the page cache (not for the
  save at shutdown, which the next start reads). Those calls are optional
  members of `aof::FileOps` that default to "not available" (the page
  cache drop to doing nothing), so a test's
  file operations keep the `fsync` path unless they override them.
- Only one child runs at a time, as in Redis: `Aof::Cron` and `Rdb::Cron`
  take `may_fork`, false while the other side's child runs, and
  `BGREWRITEAOF` and `BGSAVE` schedule or refuse as Redis' commands do.
- Pass Redis' change count to `Client::MarkModified(changes)`: what the
  command adds to `server.dirty` (one per key set or deleted, member or field
  pair given, element pushed, popped, or trimmed). It drives the save points
  and `rdb_changes_since_last_save`. Write commands, and `PING`, are refused
  with `MISCONF` through the command table's `refused_on_disk_error`.
- `Listpack` and `Intset` keep Redis' byte format in memory (little-endian
  headers, lengths, and integers, and Redis' backlen layout), since RDB files
  store them as they are; their tests compare bytes with vectors Redis
  produced. Keep any change to their encoding byte-identical to Redis'.
- `Listpack` and `Intset` have no `realloc`, so each rounds its allocation
  up to a whole allocator size class (`utils::AllocationSizeClass`,
  mimalloc's bins); the rounding costs no memory under mimalloc. `Intset`
  allocates again only when its contents leave the class. `Listpack` grows
  to the class of its new size, never further, so spare capacity is only
  what the allocator reserves anyway (past the largest class, 64 KiB, it
  grows by half), and it shrinks when a quarter full. A new buffer is not
  zero-filled, and the bytes before and after the edit are copied straight
  to their final offsets. A pair (member
  and score, field and value) goes in with one `BatchInsert` or `BatchAppend`,
  never two inserts.
- The RDB loader never trusts the file: it checks every length against the
  bytes left before allocating, validates encoded listpacks and intsets with
  `containers::ListpackReader` and the intset checks, and rejects duplicates and
  NaN scores. Like Redis, it keeps a set, hash, or sorted set listpack's bytes
  (`CreateFromListpack`), but only after one pass proves it is valid, fits
  this server's limits, holds sorted set pairs in order, and encodes each
  element as `Listpack` would; anything else is rebuilt through the data
  types. Values over the listpack limits start in their large encoding,
  sized for their count (`CreateForSize`), as Redis' rdbLoad does. Keep new
  types on that path. A count used only to pre-size (`RESIZEDB`) is capped
  by the bytes left too (`Loader::ReserveKeys`); cap any new pre-size hint
  the same way.
- Write skiplist sorted sets to RDB from the greatest member down
  (`ForEachSkiplistEntryDescending`), as Redis does, so loading inserts at
  the skiplist's head.
- Keep AOF propagation at the successful command-mutation boundary. Preserve
  command order, encode relative TTLs as absolute `PEXPIREAT` records, and do
  not copy argument payloads beyond the owned persistence record.
- `Aof::Append` only encodes into the buffer. `Server::BeforeSleep` calls
  `Aof::Flush` once per event-loop iteration, for every client that ran
  commands in it, before any of their replies are written (a group commit,
  as Redis' `beforeSleep` does). Anything that sends a reply must go through
  it: the read handler calls `Server::HoldReplies`, handed-over clients
  return to their I/O thread only after the flush, and a write callback
  skips a client that `AwaitsFlush()`. When the flush fails under `always`,
  the held replies are dropped, never sent;
  call `Flush` before arming test failures that depend on file-write order.
  Only `always` stops the server on a write or fsync error, through `Flush`
  returning false. `BGREWRITEAOF` inside `EXEC` is scheduled for `Aof::Cron`,
  so `Flush` never runs inside a transaction.
- Reply from `Server::BeforeSleep` in the iteration that processed the
  client's input, without waiting for a writable event; install a write
  callback only while output remains unsent. Read once per readable event.
- An idle client keeps no buffers: `IdleClientSweeper` calls
  `Client::ReleaseIdleBuffers` on each client once a second, from the server
  cron without threaded I/O and otherwise from the client's I/O thread,
  which skips clients handed to the main thread. Give new per-client
  buffers that grow with a request or reply a release there, and keep
  `DynamicBuffer` and `ReplyBuffer` usable after `Release()` and
  `ReleaseBuffer()`.
- Threaded I/O (`--io-threads` above 1) works like Redis 8; see
  `docs/design.md`. Keep commands, the database, and the AOF on the main
  thread, and I/O threads to the client's socket, buffers, and parser. A
  client belongs to one side at a time, and is handed over only after the
  current loop iteration. Connections close on their I/O thread; the main
  thread only frees them. Stop the I/O threads before freeing clients at
  shutdown, and pause them (`Server::PauseIoThreads`) around anything that
  must not race their allocations, such as `fork`. Run the threaded
  integration variants (`*_io_threads`) and the `thread-sanitizer` preset's
  suite after changing this path; CI runs that suite too.
- Commands reach server-wide state (the database, the AOF, the password, the
  I/O threads) through `Client::Server()`, the `ServerContext` the server
  owns. Add server-wide state there, not to `Client`.
- Rely on the command table for arity in handlers, and use
  `server/commands/lookup.h` for typed lookups instead of re-checking the
  object type and null database in each command.
- Prefer `Dict::FindOrInsert` for upserts so a key is hashed once and copied
  only when it is new.
- A string-keyed `Dict` stores each key's bytes in its entry, as Redis embeds
  keys, so a key has no allocation or `std::string` of its own. Keys are
  read back as `std::string_view` (`Dict::KeyView`), valid until the entry
  is deleted; take them that way in visitors, and insert by view
  (`FindOrInsert`) instead of building a `std::string` first. Entries keep
  the low 32 bits of the hash and a 32-bit key length, so every hash a
  `Dict` computes, and `HashedKey`'s, is truncated to 32 bits; keys stay
  within 512 MiB (the request parser's and the RDB loader's limit).
- Dict-encoded hashes store field values as `containers::CompactString`:
  16 bytes, up to 15 bytes inline and longer values in one heap block of
  exactly their length. Read them with `View()` and replace them with
  `Assign()`, which accepts a view of the string's own bytes.
- `RedisDb` operations hash their key once as a `containers::HashedKey` and
  pass it to the keyspace and WATCH dicts, which share the seeded string
  hash. Keep operations that touch both on that path instead of looking the
  key up by `std::string_view` in each.
- A key's expiration time is stored in its object's header word
  (`RedisObject::Expire()`, 0 for none), as Redis 8 keeps it in the
  kvobj, so finding a key also finds its TTL. Do not add a second lookup for
  it. `RedisDb::expires_` is only an index of the entries that have a TTL,
  for active expiration to sample: a `containers::HandleSet` of
  `Dict::EntryRef`s, one 8-byte slot per member in a table kept at most
  three-quarters full, and no allocation per key. An entry is in it exactly
  while its object's `Expire()` is not 0, so change a TTL only through
  `RedisDb::SetExpire` and `RemoveExpire`, and read one to report it
  through `RedisDb::ExpireOf`: the header has 61 bits for the time, and a
  later one (`PEXPIREAT` accepts any `int64_t`) is held as
  `RedisObject::kMaxExpire` with the exact value in `RedisDb::far_expires_`.
  Take an entry out of the index before it is deleted or extracted
  (`Dict::DeleteWith` hands the entry over just before freeing it). A key
  found through the index is deleted through `DeleteStoredKey`, which
  touches watchers before freeing the entry that holds its name.
- Active expiration is `ActiveExpire` (`server/active_expire.h`), owned by
  `Server`: the slow cycle runs from `ServerCron`, the fast one from the
  loop's before-sleep hook (`Server::BeforeSleep`, before the AOF flush, so
  the deletions it logs share that flush). Its constants follow
  Redis' `activeExpireCycle`; test it with an injected `ActiveExpire::Clock`,
  not with sleeps. The before-sleep hook runs on every loop iteration, so
  whatever is installed there must return quickly when it has nothing to do.
- String-keyed `Dict`s hash with process-seeded SipHash-1-2
  (`utils/siphash.h`), as Redis does, to resist hash flooding. Do not depend
  on iteration or `SCAN` order in tests; it changes between runs.
- `SCAN`, `HSCAN`, `SSCAN`, and `ZSCAN` share `server/commands/scan_options.h`
  (cursor and option parsing in Redis' order, and the reply) and step through
  hash tables with `Dict::ScanStep`, which follows Redis' scanGenericCommand:
  each step visits buckets until it has seen `COUNT` elements or ten times
  `COUNT` buckets. Collections in a compact encoding are returned whole with
  cursor 0, as Redis does. Keep collection encoding limits at Redis 8's
  defaults (hashes 512 entries; sets and sorted sets 128; integer sets 512;
  elements 64 bytes), since they decide reply order.
- `Skiplist` stores keys by value, orders them with a three-way comparison,
  and searches by any probe type its comparator can compare a key against. A
  lookup probe must identify at most one key; a range-bound probe may match
  many. The sorted-set skiplist searches score ranges by a bare `double`
  bound and members by a `(score, string_view)` probe; do not reintroduce
  sentinel entries, member copies for lookups, or min/max member tracking.
  Each member's name is stored once, in the member dict's entry: skiplist
  entries hold a score and an 8-byte `Dict::EntryRef` to that entry
  (`FindOrInsertEntry` returns it), so remove a member from the skiplist
  before the dict.
  Change stored keys through `Skiplist::Update`, which repositions them.
  A node and its levels are one allocation, the levels above the first
  following the node as the `level[]` array does in Redis'
  `zskiplistNode`; create and free nodes
  through `Node::Create` and `Node::Destroy`. Level 0 is the node's `next`
  pointer and stores no span, since a step there always spans one node; read
  links through `Node::Next` and `Node::Span`, which cover every level. A
  node does not store its level count: `Link` is passed it and `Unlink`
  returns it. Both savings matter because mimalloc's size classes up to 64 bytes
  are 16 bytes apart, so a sorted-set node is 32 bytes plus 16 per extra level;
  do not add a field to `Node` without measuring.
- Sorted-set range commands (`ZRANGE` and its older forms) encode replies
  from `ZSet::ForEachInRankRange`, `ForEachInScoreRange`, and
  `ForEachInLexRange`, which visit members where they are stored. They are
  templates: the skiplist encoding, which holds the large sets, calls the
  visitor directly (`ZSetSkiplist::VisitRankRange` and its siblings), since
  a `std::function` call per member measurably slows a 1,000-member range;
  the listpack encoding goes through the virtual interface. The
  vector-returning `RangeByRank`, `RangeByScore`, and `RangeByLex` copy
  entries and are for tests and tools. Do not bring back a result cache
  inside the storage: a listpack sorted set must stay as small after a range
  read as before it.
- `Listpack::ForEach`, `ForEachReverse`, and `ForEachPair` decode each
  element once through `DecodeEntry`, which yields its value and the next
  offset together. Prefer them over `Get` plus `Next` loops on read paths.
  Listpack sorted sets read integer-encoded scores directly instead of
  formatting and reparsing them.
- Keep `Quicklist` aligned with Redis' quicklist: index seeks walk nodes from
  the closer end (`Locate`), an `LSET` that overflows its node splits only
  that node, and `List` converts back to a listpack after pops, `LSET`,
  `LREM`, and `LTRIM` once one node holds at most half the size limit.
  One deliberate difference: after pops, `LSET`, `LREM`, and `LTRIM`,
  `Quicklist` merges neighboring nodes that fit together, where Redis leaves
  them. This server is I/O bound, so fewer, fuller nodes are worth the merge
  cost; keep the merges.
- Canonical integer strings are stored as `int64_t`, and other values of at
  most `EmbeddedString::kCapacity` bytes (39 with libstdc++) inside the
  `RedisObject`, like Redis' embstr, so they need no allocation of their own.
  Read string objects through `RedisObject::String()` (a `StringValue` view)
  or `Int()`, and call `MutableString()` only to edit bytes in place, since it
  converts the value back to raw form. Create objects from borrowed bytes with
  `CreateWithStringCopy`, which embeds without a temporary `std::string`.
- `RedisObject` is a hand-written tagged union, not a `std::variant`: the
  value's bytes, then one header word with a 3-bit encoding tag and the
  key's expiration time, where a variant spent a whole word on its index.
  It must stay `sizeof(EmbeddedString) + 8`; a new encoding gets a `Tag`
  and a case in `VisitValue`, and has to fit in the value's bytes. The eight
  encodings use every value of the tag, so one more also needs a fourth tag
  bit, taken from the expiration time.
- Each key's `RedisObject` lives inside its keyspace entry
  (`Dict<std::string, RedisObject>`) and holds the key's expiration time, as
  Redis 8 keeps a key, its value, and its TTL in one block, so a key is one
  allocation. Storing a value move-assigns the object into the entry, which
  replaces the value and keeps the entry's expiration time, so never keep a
  pointer into the object handed to a setter. A command that looks a key up
  and then creates or overwrites it uses `RedisDb::FindOrCreateKey` (or
  `LookupOrCreateTyped` in `server/commands/lookup.h`), which hashes the key
  and probes the keyspace once for both, and stores a string through the
  returned `KeySlot` with `SetStringAt`. A created key holds an empty string
  until the command stores its value, so nothing may return in between. Use
  `FindKeyForWrite` when a missing key must stay missing (`SET ... XX`).
  `RENAME` moves the object into the new key's entry.
- Store whole string values through `RedisDb::SetStringKey`, which overwrites
  an existing string object in place with `RedisObject::AssignString` instead
  of allocating a new object. `AssignString` applies the same encoding rules
  as `CreateWithString`, overwrites embedded values in place, reuses a small
  or well-filled raw buffer, and releases a buffer much larger than the new
  value. It accepts a value that views the object's own bytes.
- A string value of at least `RedisObject::kMinSharedStringBytes` (16 KiB) is
  held through a `std::shared_ptr`, so `GET` queues a reference to it
  (`Client::AddBulkReply(std::shared_ptr<const std::string>)`) instead of
  copying it into the reply buffer. A queued reply keeps the bytes alive and
  unchanged: never write through a buffer another holder shares.
  `AssignString` and `MutableString()` already allocate a new buffer in that
  case. Only the main thread may take a reference
  (`RedisObject::SharedStringBytes()`); I/O threads only release them.
- Only commands marked `AllowBeforeAuth` in the command table (`AUTH`,
  `HELLO`, and `QUIT`, as in Redis) run before a client authenticates when
  `--requirepass` is set.
- Inside `MULTI`, every command is queued unless the command table marks it
  `RunsImmediatelyInMulti` (`EXEC`, `DISCARD`, `MULTI`, `WATCH`, `QUIT`).
  Queued arguments are copied once into the client's `MultiState`, the
  ownership boundary for arguments that outlive a read. `EXEC` runs the queue
  under the `EXEC` command's pinned clock. The queue grows geometrically (do
  not reserve the exact size per command), and `MultiState::Reset` frees a
  queue that allocated more than 64 KiB so one large transaction does not
  stay allocated for the rest of the connection.
- `UNLINK` frees a value of more than 64 elements, and `FLUSHDB` a keyspace
  of more than 4,096 keys, on the main thread in steps:
  `RedisDb::FreeLazily` runs 10 ms per cron tick through `Dict::ClearSome`,
  `Skiplist::ClearSome`, and the collections' `ReleaseSome`. Do not move
  that to a background thread, as Redis' lazyfree does: mimalloc keeps
  memory another thread frees until the thread that allocated it collects,
  and collecting costs about as much as freeing, so the server either keeps
  the memory or stalls anyway.
- Read the time in commands through `RedisDb::Now()`, never the system clock
  directly: it is pinned per command (and per `EXEC`), as Redis'
  `commandTimeSnapshot` is, and read lazily, so commands that touch no TTL
  never read the clock.
- `WATCH` invalidation lives in `RedisDb`: its key-level mutations (set,
  delete, unlink, expire, persist, rename, flush, and expiration) touch watched
  keys. Commands that edit a value in place are covered by `Client`, which
  touches a single-key command's key after it calls `MarkModified()`. Keep new
  key-level `RedisDb` mutations touching watchers, and keep
  `TouchWatchedKey` a single branch when nothing is watched.
- Wrap an `EXEC`'s AOF records with `Aof::BeginTransaction` and
  `Aof::EndTransaction`. Replay applies a `MULTI` block only at its `EXEC` and
  truncates a block cut off at the end of the file.
- Change the AOF's base and retired files only through a new manifest,
  replaced in `PersistManifest`'s order (temporary file, sync, rename,
  directory sync), and keep the previous manifest and files until the new one
  is durable. Delete history files only after that, on the background file thread.
  Keep failed rewrites observable through `INFO persistence` without
  disabling a still-healthy AOF.
- No thread may allocate or free memory during a `fork` (the AOF rewrite's
  or `BGSAVE`'s). Pause any new thread that touches the heap through the
  server's fork hooks, which both share, as the I/O threads are.
  `BackgroundJobs` must not allocate, free, or log on its threads; reserve
  what a job needs on the thread that queues it. Keep the
  children free of locks and logging.
- `BackgroundJobs` keeps one thread and queue per job class, as Redis'
  `bio.c` does: syncs on the sync thread, closes and deletions on the file
  thread. Do not put a job that can be slow on the sync thread, and call
  `close` and `unlink` there through `FileOps` so tests can slow them down.
  Delete a file that can be large (a stopped save's temporary file, AOF
  history) through `QueueDeletion`, not inline on the main thread.
- `Dict` follows a process-wide resize policy (`SetDictResizePolicy`), as
  Redis' `dict_can_resize` does, so tables do not move entries across
  memory a child shares: the fork hooks forbid resizing in the child and set
  "avoid" in the parent, and the server cron restores it once no child
  runs. Keep new forks on those hooks.
- Commands advance a rehash one bucket per operation. The server cron
  finishes it on an idle server: `RedisDb::RehashIncrementally` runs 1 ms
  per tick (`Dict::RehashSome`), as Redis' `incrementallyRehash` does, and
  retries a shrink held back by the resize policy. It is skipped while a
  child runs; keep table upkeep added to the cron behind that check.
- Keep rewrite snapshots visitor-based, and bound snapshot memory and replay
  command sizes together (`Limits`): split large strings into replay-safe
  commands and batch collections by encoded bytes, not only element count.
- Replay the base, then the incremental files in manifest order; a listed
  file that is missing is fatal, and only the last file may end in a torn
  record. Accept `SELECT 0` and `#` annotations, as Redis writes them.
  Restarts keep appending to the last incremental file; only a rewrite starts
  a new one.

## Build And Test

Use CMake presets:

```sh
cmake --preset debug
cmake --build --preset debug
```

Run unit and integration tests separately:

```sh
ctest --preset debug -L unit --output-on-failure
ctest --preset debug -L integration --output-on-failure
```

When changing behavior shared with Redis, run the following against the
latest stable Redis release, as the `redis-differential-check` skill
describes (the last two check that each server loads the other's AOF
directory and RDB file). `scripts/build_latest_redis.sh` builds that release
without installing it, and CI's compatibility job runs the same checks with
it:

```sh
PATH="$(scripts/build_latest_redis.sh build/reference-redis):${PATH}"
scripts/run_redis_compatibility_check.sh build/debug/redis_simple
scripts/compare_with_redis.py build/debug/redis_simple
scripts/compare_side_effects.py build/debug/redis_simple
scripts/check_aof_interop.sh build/debug/redis_simple
scripts/check_rdb_interop.py build/debug/redis_simple
```

`compare_with_redis.py` compares every supported command's replies, edge
cases and random operations included; add cases when adding commands or
options. `compare_side_effects.py` compares what replies do not show: each
command's change count, whether it fails another client's `WATCH`, and the
keyspace left behind; add a case for every new write command, including ones
that change nothing and ones that fail on a missing or wrong-type key. `check_rdb_interop.py` checks that each server loads the other's RDB
file.

Run release and sanitizer checks after changes to ownership, memory layout,
assertions, or low-level data structures:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
cmake --preset sanitizer
cmake --build --preset sanitizer
ctest --preset sanitizer --output-on-failure
scripts/run_leak_check.sh
```

Run the whole suite under ThreadSanitizer after changes to threads, the
AOF's background work or `fork`, or anything they share with the main
thread. Run it serially, as the preset does: integration tests share a port.

```sh
cmake --preset thread-sanitizer
cmake --build --preset thread-sanitizer
ctest --preset thread-sanitizer --output-on-failure
```

The integration runners stop the server with `SIGTERM` and fail when it exits
nonzero, so sanitizer reports from the server (ThreadSanitizer races and
LeakSanitizer leaks, reported at exit) fail the test. Keep graceful shutdown
exiting 0.

Build and run the bounded Clang libFuzzer smoke tests after changes to request
parsing, Redis data types, core containers, buffers, expiration, persistence,
or event-loop behavior:

```sh
cmake --preset fuzz
cmake --build --preset fuzz --target redis_simple_fuzzers
ctest --preset fuzz -L fuzz --output-on-failure
```

On macOS, configure the fuzz preset with Homebrew LLVM on `PATH`; Apple Clang
does not include the libFuzzer runtime.

The leak-check script uses Apple `leaks` for the macOS unit-test binary and
explicit LeakSanitizer options for the complete Linux sanitizer test suite.
Forked children stall `leaks`, so the macOS run skips death tests,
`AofTest`, whose rewrites fork, and `RdbBackgroundSaveTest`; keep other
forking tests out of it the same way.

Use Docker for a local Linux build and test check from macOS:

```sh
scripts/run_linux_docker_check.sh
```

The helper prepares its cached Ubuntu toolchain without mounting the
repository. Its build and test container has networking disabled and mounts the
repository read-only.

Before committing, run the relevant build and tests.

## Test Layout

- Unit tests stay colocated with implementation files as `*_test.cpp`.
- Register unit tests in CTest by GoogleTest suite through
  `redis_simple_add_gtest_suite`, so failures identify the affected suite while
  preserving same-suite fixture behavior.
- Integration tests live under `integration/`. Helpers more than one of
  them needs (`Case`, `ExpectReply`, `NonEmptyLines`, `ScanAll`) live in
  `integration/client_helpers.h`; add shared ones there instead of copying
  them into each test.
- Stateful libFuzzer fuzzers live under `fuzz/`, compare operations against
  simple reference models where practical, and use bounded CTest smoke runs
  for CI.
- Current integration coverage should stay focused:
  - `integration/commands/`
  - `integration/aof_client_test.cpp`
  - `integration/rdb_client_test.cpp`
  - `integration/io_threads_client_test.cpp`
  - `integration/tcp/`
- Keep server option and graceful shutdown coverage in the lifecycle integration
  test rather than introducing a separate command-test server implementation.
- Keep `SAVE`, `BGSAVE`, `LASTSAVE`, save points, the save at shutdown,
  `MISCONF` after a failed save, loading at restart, the RDB options, and
  refusing a damaged file in the dedicated RDB integration test.
- Servers save an RDB file at shutdown and load it at startup by default, as
  Redis does, so test runners give each server its own `--dir`, and
  benchmark scripts pass `--save ""`.
- Keep AOF restart, manual and automatic rewrite compaction, shutdown flush,
  expiration, and cross-data-type recovery in the dedicated AOF integration
  test. Use injected file operations for deterministic write, sync, rename, and
  directory-sync failure unit tests.
- Keep command-family integration tests split by area, including key, string,
  set, list, zset, hash, connection, and transaction commands.
- Register integration command tests as separate CTest entries by command
  family, so failures identify the affected area without log digging.
- Keep exact ordering assertions for ordered command results such as sorted-set
  ranges; do not sort actual output in compatibility tests.
- Project runner scripts live under `scripts/`.
- Do not add manual log-inspection tests. Tests should assert behavior and
  return nonzero on failure.

## Project Management

- Run clang-format 22 on changed C/C++ files before committing or pushing. The
  format script rejects other major versions to keep local and CI output equal.
- Use `scripts/format.sh --check` and `scripts/run_clang_tidy.sh` for local
  quality checks; clang-tidy warnings are treated as errors.
- Never put required side effects inside `assert`; release builds must preserve
  behavior when assertions are disabled.
- Always update relevant docs, including the READMEs and this `CLAUDE.md`,
  when changing build, test, workflow, or project conventions. The root
  `README.md` stays short: requirements, build, run, the command coverage
  list, and links. Details live next to what they describe:
  `server/README.md` (protocol, threaded I/O, transactions),
  `server/aof/README.md`, `server/rdb/README.md`, `integration/README.md`,
  `fuzz/README.md`, `benchmarks/README.md`, `scripts/README.md` (test
  runners and checks), `scripts/measure/README.md` (measurement scripts,
  which CTest and CI do not use), and `docs/development.md` (tooling,
  style, CI). `AGENTS.md` only
  points here, so keep the guidance in this file.
- Agent skills live in `.claude/skills/<name>/SKILL.md`, and `.agents/skills`
  is a symlink to that folder, so keep one copy. Use the `pre-pr-check` skill
  before committing or opening a pull request, and update it when the checks
  in this file change. `benchmark-against-baseline` and
  `redis-differential-check` cover performance and Redis comparisons,
  `add-command` lists every place a new or changed command touches, and
  `track-redis-release` covers a new Redis release that fails CI.
- Keep CMake target-based. Source files are discovered by scoped directory
  globs in `CMakeLists.txt`; exclude generated, test, or entry-point sources
  explicitly when they do not belong in a library target.
- The `release` preset builds with link-time optimization
  (`CMAKE_INTERPROCEDURAL_OPTIMIZATION`), as Redis' release build does. Keep
  it on for that preset only, so debug, sanitizer, and fuzz builds stay fast
  to link. With LTO, GCC shares one inlining budget across the program
  (`--param inline-unit-growth`) and, once it runs out, stops inlining small
  hot helpers it inlines within a single file, which made list edits 27%
  slower; `-fopt-info-inline-missed` at link time names them. Mark such a
  helper `[[gnu::always_inline]]` (the request parser's, `SeededHash`,
  `Listpack::DecodeEntry`, the RDB encoder's `Length` and `Raw`), and
  compare an LTO build with one without it when adding hot-path code. The
  attribute goes on a function this repository defines, and what it inlines
  must not itself call a small helper out of line. A standard library call
  that a compiler leaves out of line is a different problem: the library's
  definition cannot carry the attribute, so that takes an attribute on the
  statement that makes the call.
- Keep event-loop pollers platform-selected in `CMakeLists.txt`: `kqueue` for
  macOS and `epoll` for Linux.
- The `redis_simple` executable links the bundled mimalloc submodule and
  replaces global `operator new`/`delete` through `mimalloc-new-delete.h`. Its
  only include sites are `server/main.cpp` and
  `benchmarks/mimalloc_new_delete.cpp`, which `redis_simple_benchmarks` builds
  only with `REDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC`. Each executable includes it
  at most once. Keep libc `malloc` unoverridden, keep sanitizer builds on the
  default allocator, and keep `REDIS_SIMPLE_USE_MIMALLOC=OFF` building. Allocate
  with `new` (containers, `std::make_unique`), never `malloc`, so allocations go
  through mimalloc.
  `main` sets mimalloc's purge delay to 0 (unless `MIMALLOC_PURGE_DELAY` is
  set), so freed memory returns to the OS when it is freed, as with Redis'
  allocator; with the default delay an idle server kept it indefinitely.
- Measure performance changes to the server with `scripts/measure/profile_server.sh`
  (callgrind instruction counts per workload) and memory changes with
  `scripts/measure/measure_server_memory.sh` (RSS per key), passing the previous build
  as the baseline, as the `benchmark-against-baseline` skill describes;
  `scripts/measure/compare_callgrind.py` shows which functions a regression comes from.
  Use `redis_simple_benchmarks` for changes to one component. Report instruction
  counts, not `redis-benchmark` throughput from a small or shared machine, which
  varies more than the effects being measured. Those counts miss work outside
  the request path, so also measure, against the baseline and `redis-server`:
  pauses and expiry speed with `scripts/measure/measure_pauses.py` (freeing, flushing,
  forking, active expiry), system calls with `scripts/measure/count_syscalls.py`
  (connection setup, the event loop, I/O threads), and snapshot disk writes
  with `scripts/measure/measure_snapshot_io.py` (the RDB writer and rewrites).
  `scripts/measure/compare_builds.py` summarizes throughput, latency, server CPU,
  and memory for two builds as tables for a pull request, and
  `scripts/measure/measure_server_cpu.sh` measures server CPU for one workload;
  both run on macOS, where callgrind does not.
- Keep `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml`,
  `.clang-format`, `.clang-tidy`, and `.editorconfig` aligned with project
  conventions.
- Keep CI branch pushes limited to `main`; validate feature branches through
  pull requests targeting `main`, and cancel superseded runs for the same ref.
- Avoid unrelated refactors while making focused changes.
- Do not reintroduce stale mock targets that are not part of CTest or normal
  project workflows.
