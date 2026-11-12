#!/usr/bin/env python3
"""Checks that redis_simple and redis-server each load the other's RDB file.

Each direction fills one server with every type in each of its encodings
(including edge cases such as entries at listpack size limits, binary
strings, and infinite scores), saves with SAVE, starts the other server on
the file, and compares every key: type, value, and TTL. Directions:

  redis_simple -> redis-server
  redis-server -> redis_simple (compressed and checksummed, Redis' default)
  redis-server -> redis_simple (rdbcompression no, rdbchecksum no)
  redis_simple -> redis_simple

Usage: scripts/check_rdb_interop.py REDIS_SIMPLE_BINARY
           [--redis-server PATH] [--port PORT] [--keep]

Requires a redis-server that loads RDB version 11 (Redis 7.2 or later).
Exits nonzero if a server fails to load a file or any key differs.
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


class Connection:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.file = self.sock.makefile("rb")

    @staticmethod
    def encode(args):
        out = b"*%d\r\n" % len(args)
        for arg in args:
            arg = arg if isinstance(arg, bytes) else str(arg).encode()
            out += b"$%d\r\n%s\r\n" % (len(arg), arg)
        return out

    def command(self, *args):
        self.sock.sendall(self.encode(args))
        return self.read()

    def pipeline(self, commands):
        self.sock.sendall(b"".join(self.encode(c) for c in commands))
        return [self.read() for _ in commands]

    def read(self):
        line = self.file.readline()[:-2]
        kind, rest = line[:1], line[1:]
        if kind == b"$":
            size = int(rest)
            return None if size < 0 else self.file.read(size + 2)[:-2]
        if kind == b"*":
            size = int(rest)
            return None if size < 0 else [self.read() for _ in range(size)]
        if kind == b":":
            return int(rest)
        if kind == b"-":
            return RuntimeError(rest.decode())
        return rest


def fill(connection):
    rng = random.Random(5)
    commands = [
        ["FLUSHDB"],
        ["SET", "int", "42"],
        ["SET", "negative", "-7"],
        ["SET", "int64", "-9223372036854775808"],
        ["SET", "not-canonical", "007"],
        ["SET", "short", "hello"],
        ["SET", "empty", ""],
        ["SET", "large", "z" * 2_000_000],
        ["SET", "compressible", "abc" * 500],
        ["SET", "binary", bytes(rng.randrange(256) for _ in range(5000))],
        ["SET", "ttl", "v", "PXAT", "4102444800000"],
        ["RPUSH", "list:small", "a", "1", "-5", "300", "bbb"],
        # Entries whose encoded sizes need one- to three-byte backlens,
        # including Redis' boundaries, and an element too large for a node.
        ["RPUSH", "list:long", "x" * 16378, "y" * 200, "z" * 5000,
         "w" * 100000, "q" * 2097146, "1", "-1"],
        ["SADD", "set:ints", 3, 1, -2, 100000, -9223372036854775808],
        ["SADD", "set:strings", "b", "a", "c", 10],
        ["ZADD", "zset:small", 1.5, "a", 2, "b", -3, "c", "inf", "d",
         0.1, "e", "-inf", "f"],
        ["HSET", "hash:small", "f1", "v1", "n", "12345", "neg", "-7"],
        ["HSET", "hash:long", "f", "v" * 100],
        ["HSET", "hash:ttl", "a", "b"],
        ["PEXPIREAT", "hash:ttl", "4102444800000"],
    ]
    for i in range(3000):
        commands.append(["RPUSH", "list:big", f"element-{i}" if i % 3 else i * 1000])
    for i in range(300):
        commands.append(["SADD", "set:big", f"member-{i}"])
        commands.append(["ZADD", "zset:big", repr(rng.uniform(-1e9, 1e9)), f"member-{i}"])
    for i in range(600):
        commands.append(["SADD", "set:bigints", i * 7])
        commands.append(["HSET", "hash:big", f"field-{i}", f"value-{i}"])
    for reply in connection.pipeline(commands):
        if isinstance(reply, Exception):
            raise reply


def snapshot(connection):
    keys, cursor = set(), b"0"
    while True:
        cursor, batch = connection.command("SCAN", cursor, "COUNT", 1000)
        keys.update(batch)
        if cursor == b"0":
            break
    result = {}
    for key in sorted(keys):
        kind = connection.command("TYPE", key)
        if kind == b"string":
            value = connection.command("GET", key)
        elif kind == b"list":
            value = connection.command("LRANGE", key, 0, -1)
        elif kind == b"set":
            value = sorted(connection.command("SMEMBERS", key))
        elif kind == b"zset":
            # By value: the file stores the double; how replies print it is
            # compare_with_redis.py's concern.
            pairs = connection.command("ZRANGE", key, 0, -1, "WITHSCORES")
            value = [(m, float(s)) for m, s in zip(pairs[::2], pairs[1::2])]
        elif kind == b"hash":
            value = sorted(connection.command("HGETALL", key))
        else:
            value = None
        ttl = connection.command("PTTL", key)
        # Whole thousands of seconds, so the time the check takes is ignored.
        result[key] = (kind, value, ttl if ttl < 0 else round(ttl / 1e6))
    return result


class Server:
    def __init__(self, command, port, directory, log):
        self.port = port
        self.log = log
        with open(log, "wb") as output:
            self.process = subprocess.Popen(command, cwd=directory, stdout=output,
                                            stderr=subprocess.STDOUT)
        for _ in range(100):
            if self.process.poll() is not None:
                raise RuntimeError(f"server on port {port} exited; see {log}")
            try:
                if Connection(port).command("PING") == b"PONG":
                    return
            except OSError:
                pass
            time.sleep(0.1)
        raise RuntimeError(f"server on port {port} did not start; see {log}")

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=30)


def start(kind, binary, port, directory):
    log = os.path.join(directory, f"{kind}-{port}.log")
    if kind == "simple":
        command = [binary, "--port", str(port), "--dir", directory]
    else:
        command = [binary, "--port", str(port), "--dir", directory, "--save", "",
                   "--appendonly", "no", "--enable-debug-command", "no"]
    return Server(command, port, directory, log)


def run_direction(name, source, target, binaries, port, root, configure=()):
    directory = tempfile.mkdtemp(prefix="rdb_", dir=root)
    writer = start(source, binaries[source], port, directory)
    try:
        connection = Connection(port)
        for config in configure:
            connection.command("CONFIG", "SET", *config)
        fill(connection)
        if connection.command("SAVE") != b"OK":
            raise RuntimeError("SAVE failed")
        expected = snapshot(connection)
    finally:
        writer.stop()
    reader = start(target, binaries[target], port, directory)
    try:
        actual = snapshot(Connection(port))
    finally:
        reader.stop()
    differences = [k for k in sorted(set(expected) | set(actual))
                   if expected.get(k) != actual.get(k)]
    for key in differences[:10]:
        print(f"  {key!r}: wrote {str(expected.get(key))[:120]}, "
              f"loaded {str(actual.get(key))[:120]}")
    status = "ok" if not differences else f"{len(differences)} differences"
    print(f"{name}: {len(expected)} keys, {status}")
    return not differences


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--redis-server", default="redis-server")
    parser.add_argument("--port", type=int, default=8610)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    redis_server = shutil.which(args.redis_server)
    if redis_server is None:
        sys.exit(f"{args.redis_server} not found")
    binaries = {"simple": os.path.abspath(args.binary), "redis": redis_server}
    root = tempfile.mkdtemp(prefix="redis_simple_rdb_interop.")
    ok = True
    try:
        ok &= run_direction("redis_simple -> redis-server", "simple", "redis",
                            binaries, args.port, root)
        ok &= run_direction("redis-server -> redis_simple", "redis", "simple",
                            binaries, args.port, root)
        ok &= run_direction(
            "redis-server (no compression or checksum) -> redis_simple",
            "redis", "simple", binaries, args.port, root,
            configure=[("rdbcompression", "no"), ("rdbchecksum", "no")])
        ok &= run_direction("redis_simple -> redis_simple", "simple", "simple",
                            binaries, args.port, root)
    finally:
        if args.keep:
            print(f"kept {root}")
        else:
            shutil.rmtree(root, ignore_errors=True)
    if not ok:
        sys.exit(1)
    print("RDB interoperability checks passed")


if __name__ == "__main__":
    main()
