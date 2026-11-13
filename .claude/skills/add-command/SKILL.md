---
name: add-command
description: Add a Redis command to this server, or change an existing one's options or behavior, without missing one of the places a command touches (table, handler, change count, AOF, tests, differential scripts, docs).
---

# Add or change a command

`CLAUDE.md` holds the rules; this is the list of places, in the order to
visit them. Read the command's implementation in the latest Redis release
first (`src/t_*.c`, `src/db.c`, `src/expire.c`): the order in which it
checks arguments, the key's type, and options decides which error a bad
request gets, and the differential scripts compare that.

## 1. Table and handler

1. Add a row to the sorted table in `server/commands/command.cpp`: name
   (upper-case letters, at most 16 bytes), arity, access mode, and key
   positions. Mark it there if it runs before `AUTH`, runs immediately inside
   `MULTI`, or is refused with `MISCONF` after a failed save.
2. Declare the handler in `server/commands/handlers.h` and implement it in
   the command family's file, `server/commands/<family>.cpp` (`key.cpp`,
   `string.cpp`, `list.cpp`, `set.cpp`, `zset.cpp`, `hash.cpp`), in the
   namespace of the same name (`commands::set`).
3. Rely on the table for arity. Look keys up with `server/commands/lookup.h`.
   Parse integers with `utils::ToInt64` and floats with `utils::ToDouble`.
4. Read arguments as `std::string_view`s and copy only what is stored.
   Encode collection replies from visitors, not from vectors of copies.

## 2. Side effects of a write

1. Call `Client::MarkModified(changes)` with what Redis adds to
   `server.dirty`, and only when something changed. A command that changes
   nothing must not reach the AOF.
2. Check what Redis does for a no-op: some commands still signal the key,
   which fails a `WATCH` (`client->Db()->TouchWatchedKey`), and some do
   nothing at all.
3. Check that the AOF can replay it: `server/aof/records.cpp` encodes
   mutations, and relative expirations are written as `PEXPIREAT`.
4. Delete a collection key when the command empties it.

## 3. Tests

1. Add cases to the command family's test in `integration/commands/`.
2. Add edge cases, and the command to the random operations when it fits,
   in `scripts/compare_with_redis.py`.
3. For a write command, add cases to `scripts/compare_side_effects.py`,
   including ones that change nothing, and add the command to the list in
   `scripts/check_aof_interop.sh`.
4. For a new data-structure operation, add unit tests beside it and extend
   the matching harness under `fuzz/`.

## 4. Docs

1. Update the command coverage list in `README.md`.
2. Update `CLAUDE.md` when the command adds a convention, and
   `docs/design.md` when it changes the request path, the AOF, or RDB.

## 5. Check

Run the `redis-differential-check` skill against the latest Redis release,
then the `pre-pr-check` skill.
