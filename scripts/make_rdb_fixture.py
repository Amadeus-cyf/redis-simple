#!/usr/bin/env python3
"""Writes the RDB file RdbTest loads, with a redis-server of any version.

Fills a fresh redis-server with every type in each of its encodings,
LZF-compressed strings, expirations, a key that has already expired, and a
hash listpack whose values sit at backlen boundaries, then saves it with
SAVE. RdbTest.LoadsAFileRedisWrote expects exactly this keyspace from each
file in server/rdb/testdata/, so add a file when Redis changes what it
writes:

  scripts/make_rdb_fixture.py PATH/redis-server server/rdb/testdata/redis_X_Y.rdb

Requires Redis 7.0 or later (DEBUG SET-ACTIVE-EXPIRE behind
enable-debug-command).
"""
import argparse
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

# The time RdbTest pretends it is, minus one: keys expiring then are kept.
FUTURE_MS = 4102444800000


class Connection:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.file = self.sock.makefile("rb")

    def command(self, *args):
        out = b"*%d\r\n" % len(args)
        for arg in args:
            arg = arg if isinstance(arg, bytes) else str(arg).encode()
            out += b"$%d\r\n%s\r\n" % (len(arg), arg)
        self.sock.sendall(out)
        line = self.file.readline()[:-2]
        if line[:1] == b"-":
            raise RuntimeError(f"{args[0]}: {line[1:].decode()}")
        if line[:1] == b"$":
            size = int(line[1:])
            return None if size < 0 else self.file.read(size + 2)[:-2]
        return line[1:]


def fill(connection):
    rng = random.Random(11)
    commands = [
        ["SET", "int:small", 42],
        ["SET", "int:big", 9223372036854775807],
        ["SET", "str:short", "hello"],
        ["SET", "str:empty", ""],
        # Long and repetitive enough for LZF.
        ["SET", "str:compressible", "abcabcabc" * 20],
        # Too random for LZF to shorten.
        ["SET", "str:random", "".join(
            rng.choice("abcdefghijklmnopqrstuvwxyz0123456789")
            for _ in range(300))],
        ["RPUSH", "list:small", "a", 1, -5, 300, "bbb"],
        ["RPUSH", "list:big", *(f"element-{i}-" + "x" * 20
                                for i in range(1, 601))],
        ["SADD", "set:ints", 3, 1, -2, 100000],
        ["SADD", "set:strings", "b", "a", "c", 10],
        ["SADD", "set:big", *(f"member-{i}" for i in range(1, 201))],
        ["SADD", "set:bigints", *range(1, 601)],
        ["ZADD", "zset:small", -3, "c", 1.5, "a", 2, "b", "inf", "d"],
        ["ZADD", "zset:big", *(x for i in range(1, 201)
                               for x in (f"{i}.25", f"member-{i}"))],
        ["HSET", "hash:small", "f1", "v1", "n", 12345],
        ["HSET", "hash:longvalue", "f", "v" * 100],
        ["HSET", "hash:big", *(x for i in range(1, 601)
                               for x in (f"field-{i}", f"value-{i}"))],
        ["SET", "expiring:future", "v"],
        ["PEXPIREAT", "expiring:future", FUTURE_MS],
        ["HSET", "expiring:hash", "a", "b"],
        ["PEXPIREAT", "expiring:hash", FUTURE_MS],
    ]
    for command in commands:
        connection.command(*command)
    # Values whose encoded sizes are 16383 and 2097151 bytes (a 5-byte string
    # header each), where Redis 8.8 to 8.10 wrote a backlen one byte shorter
    # than other releases. Only a hash, set, or sorted set listpack holds
    # them: Redis gives a list element this large a node of its own.
    connection.command("CONFIG", "SET", "hash-max-listpack-value", 3000000)
    connection.command("HSET", "hash:boundary", "first", "a" * 16378,
                       "second", "b" * 2097146)
    connection.command("CONFIG", "SET", "hash-max-listpack-value", 64)
    # A key that has expired but is still in memory, so SAVE writes it:
    # nothing reads it, and active expiration is off.
    connection.command("DEBUG", "SET-ACTIVE-EXPIRE", 0)
    connection.command("SET", "expired:past", "gone", "PX", 1)
    time.sleep(0.05)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("redis_server")
    parser.add_argument("output")
    parser.add_argument("--port", type=int, default=8620)
    args = parser.parse_args()

    directory = tempfile.mkdtemp(prefix="redis_simple_rdb_fixture.")
    server = subprocess.Popen(
        [args.redis_server, "--port", str(args.port), "--bind", "127.0.0.1",
         "--dir", directory, "--save", "", "--appendonly", "no",
         "--enable-debug-command", "local"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(200):
            try:
                connection = Connection(args.port)
                break
            except OSError:
                time.sleep(0.05)
        else:
            sys.exit("redis-server did not start")
        version = connection.command("INFO", "server").split(b"redis_version:")
        fill(connection)
        if connection.command("SAVE") != b"OK":
            sys.exit("SAVE failed")
        shutil.copyfile(os.path.join(directory, "dump.rdb"), args.output)
        print(f"wrote {args.output} with redis-server "
              f"{version[1].split(b'\r\n')[0].decode()}")
    finally:
        server.terminate()
        server.wait()
        shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    main()
