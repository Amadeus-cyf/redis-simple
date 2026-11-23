# RDB snapshots

How the server saves and loads RDB files.

## Saving

RDB snapshots follow Redis 8.12's `src/rdb.c`. `SAVE` writes the database to
`dump.rdb` (`--dbfilename`) in the working directory (`--dir`, which also holds
the AOF): first as `temp-<pid>.rdb`, synced, then renamed over the file with
the directory synced, so a crash never leaves a partial file in its place.
`BGSAVE` does the same in a forked child while the server keeps serving. As
in Redis, one child runs at a time: `BGSAVE SCHEDULE` waits for a running AOF
rewrite, `BGREWRITEAOF` waits for a running save, and inside `MULTI` both are
scheduled for after the transaction. `LASTSAVE` reports when the last save
finished. At startup, when the AOF is off, the server loads the file if
there is one, and refuses to start if it is damaged.

## Save points

Save points start background saves on their own, as Redis' `save` does:
`--save "<seconds> <changes> ..."` saves once that many changes were made and
more than that many seconds have passed since the last save. The default is
Redis 8's, `3600 1 300 100 60 10000`. The first `--save` replaces it and later
ones add pairs; `--save ""` turns saving off. Changes are counted as Redis
counts them (one per key, member, field, or element set or removed), and
`INFO persistence` reports them with Redis' other `rdb_` fields, and
`INFO stats` reports `latest_fork_usec` and `total_forks` for the forks of
`BGSAVE` and AOF rewrites. With save
points, the server also saves when it shuts down, and keeps running if that
save fails. After a failed background save, write commands and `PING` are
refused with Redis' `MISCONF` error until a save succeeds, unless
`--stop-writes-on-bgsave-error no`.

```sh
./build/debug/redis_simple --dir /var/lib/redis_simple --dbfilename dump.rdb \
  --save "900 1 300 10"
```

## File format and loading

The file is in Redis' format, and each server loads the other's. Values are
stored in the encodings Redis stores them in, listpacks and intsets byte for
byte as they are in memory; strings over 20 bytes are LZF-compressed when
that saves space (`--rdbcompression`), and the file ends with a CRC64
(`--rdbchecksum`). It is written as RDB version 11, the oldest that covers
its contents, so Redis 7.2 and later load it; Redis 8.12 writes version 17.
This server loads files from Redis 7.0 and later, 8.12's included, and the
plain encodings of older versions. It refuses what it does not have: streams,
modules, hash field expiration, set member expiration, bitmaps, rate limiters,
hash templates, keys outside database 0, and the ziplist encodings of Redis
6.2 and earlier; it skips function libraries with a warning, and ignores
Redis 8.12's mark that a key is exempt from eviction (`BLESS`), since it
evicts nothing.

Redis 8.8 to 8.10 wrote the backlen of a listpack element whose encoded size
is 16383, 2097151, or 268435455 bytes one byte shorter than every other
release, and so did earlier builds of this server. Redis 8.12 went back to
the usual width, as this server has, and widens the short ones when it loads
RDB version 14 or 15, which Redis 8.8 to 8.10 wrote. This server widens them
in every version, since its earlier builds wrote version 11
(`containers::WidenCompactBacklens`). Redis 8.12 cannot read such a file from
an earlier build of this server: save it again with this one. Keys whose time has passed are dropped while loading. Every
length, encoded listpack and intset, and duplicate is checked, so a corrupt
file is refused, never trusted. As in Redis, a set, hash, or sorted set
listpack that passes those checks and fits this server's limits is kept as
stored, so loading it is a copy rather than one insert per element.
