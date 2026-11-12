#!/usr/bin/env python3
"""Compares redis_simple's replies with a reference redis-server.

Starts both servers, sends each the same commands, and lists every command
whose replies differ. The commands are fixed edge cases for every supported
command (boundary indices and counts, invalid numbers, wrong types, wrong
arities, option combinations), then seeded random operations over a few keys
whose collections grow past their compact encodings. Each set runs in RESP2
and RESP3.

Set, hash, and SMEMBERS-style replies are compared as sets. When the
reference is older than Redis 8, non-integer numbers are compared by value,
since Redis 7 prints them with %.17g and Redis 8 in shortest form.

Usage: scripts/compare_with_redis.py REDIS_SIMPLE_BINARY [options]
"""

import argparse
import itertools
import os
import random
import shlex
import socket
import subprocess
import sys
import tempfile
import time


def edge_cases():
    out = []

    def add(*lines):
        out.extend(lines)

    # Setup of each type, plus a wrong-type key for every type.
    add(
        "FLUSHDB",
        "SET s hello",
        "SET n 10",
        "SET neg -5",
        "SET f 3.5",
        "SET big 9223372036854775807",
        "SET small -9223372036854775808",
        "RPUSH l a b c d e f g h",
        "SADD st x y z 1 2 3",
        "SADD st2 y z w",
        "SADD ist 1 2 3 4 5",
        "SADD ist2 3 4 5 6",
        "ZADD z 1 a 2 b 3 c 4 d 5 e 1 aa 1 ab",
        "ZADD zl 0 a 0 b 0 c 0 d 0 e 0 f 0 g",
        "HSET h f1 v1 f2 v2 f3 10",
    )

    ints = [
        "0",
        "1",
        "-1",
        "2",
        "-2",
        "100",
        "-100",
        "+1",
        "01",
        "-0",
        "1.0",
        "abc",
        "",
        "9223372036854775807",
        "-9223372036854775808",
        "9223372036854775808",
        " 1",
    ]
    floats = [
        "0",
        "1.5",
        "-1.5",
        "inf",
        "-inf",
        "+inf",
        "nan",
        "1e3",
        "abc",
        "",
        "0x10",
        "1e400",
        ".5",
        "5.",
        "1,5",
    ]

    types = {
        "s": "string",
        "l": "list",
        "st": "set",
        "z": "zset",
        "h": "hash",
        "missing": "none",
    }

    # Strings
    for key in ["s", "n", "missing", "l"]:
        add(f"GET {key}", f"STRLEN {key}", f"TYPE {key}", f"EXISTS {key}")
    add("GET", "GET a b", "STRLEN", "EXISTS s n missing s", "EXISTS")
    for key in ["n", "neg", "big", "small", "s", "f", "missing", "l"]:
        add(f"SET tmp_{key} x", f"DEL tmp_{key}")
    for v in ints:
        add(f"SET ic 5", f'INCRBY ic "{v}"', f'DECRBY ic "{v}"', "GET ic")
    add(
        "INCR big",
        "DECR small",
        "INCR s",
        "INCR f",
        "INCR l",
        "DECR missing_dec",
        "INCR",
    )
    add(
        "APPEND s ' world'",
        "GET s",
        "APPEND l x",
        "APPEND newapp ''",
        "EXISTS newapp",
        "APPEND",
    )
    add(
        "MSET",
        "MSET a",
        "MSET a 1 b",
        "MSET a 1 b 2",
        "MGET a b missing l s",
        "MGET",
    )
    for opt in [
        "EX 10",
        "PX 10000",
        "EX 0",
        "EX -1",
        "PX 0",
        "EX abc",
        "EX",
        "NX",
        "XX",
        "NX XX",
        "EX 10 PX 10",
        "KEEPTTL",
        "KEEPTTL EX 10",
        "GET",
        "GET NX",
        "EXAT 4102444800",
        "PXAT 4102444800000",
        "EXAT 0",
        "EX 9223372036854775807",
        "PX 9223372036854775807",
        "ex 10",
        "FOO",
    ]:
        add(f"SET so v {opt}", "GET so", "TTL so")
    add("SET s2 v GET", "SET l v GET", "SET")

    # Keys
    for key in ["s", "l", "missing"]:
        add(f"TTL {key}", f"PTTL {key}", f"PERSIST {key}")
    for cmd in ["EXPIRE", "PEXPIRE", "EXPIREAT", "PEXPIREAT"]:
        for v in [
            "100",
            "0",
            "-1",
            "abc",
            "9223372036854775807",
            "4102444800",
        ]:
            add(f"SET e v", f"{cmd} e {v}", "TTL e", "EXISTS e")
        for opt in [
            "NX",
            "XX",
            "GT",
            "LT",
            "NX XX",
            "GT LT",
            "NX GT",
            "FOO",
            "nx",
        ]:
            add(
                "SET e2 v",
                f"{cmd} e2 1000 {opt}",
                f"{cmd} e2 2000 {opt}",
                "TTL e2",
            )
        add(f"{cmd} missing 10", f"{cmd} e")
    add(
        "RENAME s s",
        "RENAME missing x",
        "SET r1 a",
        "RENAME r1 r2",
        "GET r2",
        "RENAME r2",
        "RENAME",
    )
    add("SET d1 1", "SET d2 2", "DEL d1 d2 missing", "UNLINK missing", "DEL")
    add("DBSIZE", "DBSIZE x", "TYPE", "TYPE a b")
    # SCAN with a single matching key or a complete pass, so the order of
    # keys, which depends on the hash seed, does not matter.
    add(
        "SCAN 0 MATCH s COUNT 1000",
        "SCAN 0 MATCH s TYPE string COUNT 1000",
        "SCAN 0 MATCH s TYPE STRING COUNT 1000",
        "SCAN 0 MATCH l TYPE list COUNT 1000",
        "SCAN 0 MATCH s TYPE list COUNT 1000",
        "SCAN 0 TYPE nosuch COUNT 1000",
        "SCAN +0 MATCH s COUNT 1000",
        "SCAN 000 MATCH s COUNT 1000",
        "SCAN -1",
        "SCAN abc",
        "SCAN 18446744073709551616",
        "SCAN 0 COUNT",
        "SCAN 0 MATCH",
        "SCAN 0 TYPE",
        "SCAN 0 COUNT 0",
        "SCAN 0 COUNT -1",
        "SCAN 0 COUNT x",
        "SCAN 0 NOVALUES",
        "SCAN 0 COUNT 10 NOVALUES",
        "SCAN 0 FOO bar",
    )
    add("NOTACOMMAND", "NOTACOMMAND a b", "notacommand " + "x" * 200)
    # SAVE replies OK, rejects arguments, and is not allowed inside MULTI,
    # which then aborts. LASTSAVE's time differs between the servers, so only
    # its arity is compared.
    add("SAVE", "SAVE now", "LASTSAVE now")
    add("MULTI", "SET in-multi v", "SAVE", "EXEC", "EXISTS in-multi")
    # BGSAVE takes only SCHEDULE. With no save running it starts one; cases
    # that depend on whether that child has exited yet are left to the RDB
    # integration test.
    add("BGSAVE now", "BGSAVE schedule x", "BGSAVE")
    # HSCAN, SSCAN, and ZSCAN on small encodings, which Redis returns whole
    # and in a fixed order, and their errors. A missing key replies before
    # the options are checked, and a wrong type before them too.
    for command, key in (("HSCAN", "h"), ("SSCAN", "st"), ("SSCAN", "ist"),
                         ("ZSCAN", "z"), ("ZSCAN", "zl")):
        add(
            f"{command} {key} 0",
            f"{command} {key} 5 COUNT 1",
            f"{command} {key} 0 MATCH *a*",
            f"{command} {key} 0 MATCH [x-z] COUNT 100",
            f"{command} {key} 0 NOVALUES",
            f"{command} {key} 0 novalues MATCH f*",
            f"{command} {key} 0 TYPE string",
            f"{command} {key} 0 COUNT 0",
            f"{command} {key} 0 COUNT x",
            f"{command} {key} 0 MATCH",
            f"{command} {key} +0",
            f"{command} {key} -1",
            f"{command} {key} abc",
            f"{command} {key}",
            f"{command} missing 0 BAD",
            f"{command} missing abc",
            f"{command} s 0",
            f"{command} s 0 BAD",
        )
    add("ECHO hi", "ECHO", "ECHO a b", "PING", "PING x", "PING x y")

    # Lists
    for v in ints:
        add(f'LINDEX l "{v}"')
    for a, b in itertools.product(
        ["0", "-1", "2", "-3", "100", "-100", "x"], repeat=2
    ):
        add(f"LRANGE l {a} {b}")
    add(
        "LRANGE missing 0 -1",
        "LRANGE s 0 -1",
        "LINDEX s 0",
        "LINDEX missing 0",
        "LINDEX missing x",
        "LLEN l",
        "LLEN missing",
        "LLEN s",
        "LLEN",
    )
    for v in ["0", "-1", "7", "8", "-9", "x"]:
        add(
            "DEL lw",
            "RPUSH lw a b c d e f g h",
            f"LSET lw {v} new",
            "LRANGE lw 0 -1",
        )
    add("LSET missing 0 x", "LSET s 0 x")
    for cnt in ["0", "1", "-1", "2", "-2", "100", "x"]:
        add(
            "DEL lr",
            "RPUSH lr a b a c a d a",
            f"LREM lr {cnt} a",
            "LRANGE lr 0 -1",
        )
    add("LREM missing 1 a", "LREM s 1 a")
    for a, b in [
        ("0", "-1"),
        ("1", "2"),
        ("-2", "-1"),
        ("5", "2"),
        ("100", "200"),
        ("-100", "100"),
        ("x", "1"),
    ]:
        add(
            "DEL lt",
            "RPUSH lt a b c d e",
            f"LTRIM lt {a} {b}",
            "LRANGE lt 0 -1",
            "EXISTS lt",
        )
    for cmd in ["LPOP", "RPOP"]:
        for cnt in ["", "0", "1", "3", "100", "-1", "x"]:
            add(
                "DEL lp",
                "RPUSH lp a b c d",
                f"{cmd} lp {cnt}",
                "LRANGE lp 0 -1",
                "EXISTS lp",
            )
        add(
            f"{cmd} missing",
            f"{cmd} missing 2",
            f"{cmd} missing 0",
            f"{cmd} s",
            f"{cmd} s 1",
        )
    add(
        "LPUSH s x",
        "RPUSH s x",
        "LPUSH nl a b c",
        "LRANGE nl 0 -1",
        "LPUSH",
        "LPUSH nl",
    )

    # Sets
    add(
        "SCARD st",
        "SCARD missing",
        "SCARD s",
        "SISMEMBER st x",
        "SISMEMBER st q",
        "SISMEMBER missing x",
        "SISMEMBER s x",
        "SADD st x",
        "SADD s x",
        "SREM st q",
        "SREM missing x",
        "SREM s x",
        "SCARD ist",
        "SISMEMBER ist 3",
        "SISMEMBER ist 03",
        "SADD ist 7 x",
        "SCARD ist",
    )
    for cmd in ["SINTER", "SUNION", "SDIFF"]:
        for keys in [
            "st st2",
            "st missing",
            "missing st",
            "st s",
            "ist ist2",
            "st",
            "missing",
        ]:
            add(f"{cmd} {keys}")
    add("SMEMBERS missing", "SMEMBERS s")

    # Sorted sets
    add(
        "ZCARD z",
        "ZCARD missing",
        "ZCARD s",
        "ZSCORE z a",
        "ZSCORE z q",
        "ZSCORE missing a",
        "ZSCORE s a",
    )
    for m in ["a", "e", "aa", "q"]:
        add(f"ZRANK z {m}", f"ZREVRANK z {m}")
    add("ZRANK missing a", "ZRANK s a")
    for a, b in itertools.product(
        ["0", "-1", "2", "-3", "100", "-100", "x"], repeat=2
    ):
        add(f"ZRANGE z {a} {b}", f"ZREVRANGE z {a} {b} WITHSCORES")
    bounds = [
        "-inf",
        "+inf",
        "1",
        "(1",
        "2.5",
        "(5",
        "5",
        "abc",
        "(abc",
        "",
        "inf",
        "-inf",
        "(-inf",
        "nan",
    ]
    for a, b in itertools.product(bounds, repeat=2):
        add(f'ZRANGEBYSCORE z "{a}" "{b}"', f'ZCOUNT z "{a}" "{b}"')
    for a, b in [("-inf", "+inf"), ("+inf", "-inf"), ("5", "1"), ("(5", "(1")]:
        add(f"ZREVRANGEBYSCORE z {a} {b} WITHSCORES")
    for lim in [
        "LIMIT 0 2",
        "LIMIT 1 -1",
        "LIMIT -1 2",
        "LIMIT 0 0",
        "LIMIT 2",
        "LIMIT x 1",
        "WITHSCORES LIMIT 1 2",
    ]:
        add(
            f"ZRANGEBYSCORE z -inf +inf {lim}",
            f"ZRANGE z 0 -1 BYSCORE {lim}",
            f"ZRANGE z -inf +inf BYSCORE {lim}",
        )
    lex = ["-", "+", "[a", "(a", "[c", "(c", "[aa", "a", "", "[", "(", "[z"]
    for a, b in itertools.product(lex, repeat=2):
        add(f'ZRANGEBYLEX zl "{a}" "{b}"', f'ZLEXCOUNT zl "{a}" "{b}"')
    for a, b in [("+", "-"), ("[e", "[b"), ("(e", "-")]:
        add(
            f"ZREVRANGEBYLEX zl {a} {b}",
            f"ZREVRANGEBYLEX zl {a} {b} LIMIT 1 2",
        )
    for opts in [
        "BYLEX",
        "BYLEX REV",
        "REV",
        "BYSCORE REV",
        "BYLEX WITHSCORES",
        "BYSCORE BYLEX",
        "REV REV",
        "LIMIT 0 1",
        "BYLEX LIMIT 1 2",
        "WITHSCORES",
        "FOO",
    ]:
        add(
            f"ZRANGE zl [a [e {opts}",
            f"ZRANGE z 0 3 {opts}",
            f"ZRANGE z 5 1 {opts}",
        )
    for f in floats:
        add(f'ZADD zf "{f}" m', "ZSCORE zf m")
    for opts in [
        "NX",
        "XX",
        "GT",
        "LT",
        "CH",
        "INCR",
        "NX XX",
        "GT LT",
        "NX GT",
        "GT CH",
        "LT CH",
        "XX CH",
        "INCR NX",
        "INCR XX",
        "INCR GT",
        "XX GT CH",
        "nx",
    ]:
        add(
            "DEL zo",
            "ZADD zo 5 a 5 b",
            f"ZADD zo {opts} 10 a 1 c",
            f"ZADD zo {opts} 1 a",
            "ZRANGE zo 0 -1 WITHSCORES",
        )
    add(
        "ZADD zo",
        "ZADD zo 1",
        "ZADD zo 1 a 2",
        "ZADD zo XX",
        "ZADD missing XX 1 a",
        "EXISTS missing",
        "ZADD zi INCR 1 a",
        "ZADD zi INCR inf a",
        "ZADD zi INCR -inf a",
        "ZADD s 1 a",
        "ZADD zr 1 a 2 b",
        "ZREM zr a q",
        "ZREM zr b",
        "EXISTS zr",
        "ZREM missing a",
        "ZREM s a",
    )

    # Hashes
    add(
        "HGET h f1",
        "HGET h q",
        "HGET missing f",
        "HGET s f",
        "HEXISTS h f1",
        "HEXISTS h q",
        "HLEN h",
        "HLEN missing",
        "HLEN s",
        "HMGET h f1 q f2",
        "HMGET missing a",
        "HMGET s a",
        "HKEYS missing",
        "HVALS missing",
        "HGETALL missing",
        "HKEYS s",
        "HVALS s",
        "HGETALL s",
        "HSET h",
        "HSET h f",
        "HSET h f v g",
        "HMSET h f",
        "HMSET h a 1 b 2",
        "HSET s f v",
        "HDEL h q",
        "HDEL missing f",
        "HDEL s f",
    )
    for v in ints:
        add("HSET hi f 5", f'HINCRBY hi f "{v}"', "HGET hi f")
    add(
        "HSET hi g abc",
        "HINCRBY hi g 1",
        "HSET hi m 9223372036854775807",
        "HINCRBY hi m 1",
        "HINCRBY hi newf 3",
        "HINCRBY missinghash f 2",
        "HINCRBY s f 1",
    )

    # Transactions
    add(
        "MULTI",
        "SET t 1",
        "INCR t",
        "LPUSH t x",
        "GET t",
        "EXEC",
        "MULTI",
        "SET t2 1",
        "FOO",
        "EXEC",
        "EXEC",
        "DISCARD",
        "MULTI",
        "MULTI",
        "DISCARD",
        "MULTI",
        "SET t3 1",
        "INCR",
        "EXEC",
        "WATCH s",
        "SET s changed",
        "MULTI",
        "GET s",
        "EXEC",
        "WATCH s",
        "MULTI",
        "GET s",
        "EXEC",
        "UNWATCH",
        "MULTI",
        "WATCH x",
        "DISCARD",
    )

    # SELECT 1 is left out: this server has one database.
    add("SELECT 0", "SELECT x", "SELECT -1", "FLUSHDB", "DBSIZE")

    return out


ZADD_FLAGS = ["GT", "LT", "XX", "NX", "CH"]


def random_operations(seed, count):
    random.seed(seed)
    keys = [f"k{i}" for i in range(6)]
    members = (
        [f"m{i}" for i in range(300)]
        + [str(i) for i in range(-50, 600)]
        + ["x" * 80, "y" * 70]
    )

    def member():
        return random.choice(members)

    def score():
        return random.choice(
            [
                str(random.randint(-20, 20)),
                f"{random.uniform(-10, 10):.3f}",
                "0",
                "-0",
                "inf",
                "-inf",
                "1.5",
            ]
        )

    def index():
        return str(random.choice([random.randint(-400, 400), 0, -1, 1]))

    def ops():
        key = random.choice(keys)
        other_key = random.choice(keys)
        many = " ".join(member() for _ in range(random.randint(1, 30)))
        pairs = " ".join(
            f"{score()} {member()}" for _ in range(random.randint(1, 20))
        )
        fields = " ".join(
            f"{member()} {member()}" for _ in range(random.randint(1, 20))
        )
        choices = [
            f"SET {key} {member()}",
            f"APPEND {key} {member()}",
            f"INCR {key}",
            f"GET {key}",
            f"STRLEN {key}",
            f"RPUSH {key} {many}",
            f"LPUSH {key} {many}",
            f"LPOP {key}",
            f"RPOP {key} {random.randint(0, 40)}",
            f"LRANGE {key} {index()} {index()}",
            f"LINDEX {key} {index()}",
            f"LSET {key} {index()} {member()}",
            f"LREM {key} {random.randint(-3, 3)} {member()}",
            f"LTRIM {key} {index()} {index()}",
            f"LLEN {key}",
            f"SADD {key} {many}",
            f"SREM {key} {many}",
            f"SCARD {key}",
            f"SISMEMBER {key} {member()}",
            f"SMEMBERS {key}",
            f"SINTER {key} {other_key}",
            f"SUNION {key} {other_key}",
            f"SDIFF {key} {other_key}",
            f"ZADD {key} {pairs}",
            f"ZADD {key} {random.choice(ZADD_FLAGS)} {pairs}",
            f"ZADD {key} INCR {score()} {member()}",
            f"ZREM {key} {many}",
            f"ZCARD {key}",
            f"ZSCORE {key} {member()}",
            f"ZRANK {key} {member()}",
            f"ZREVRANK {key} {member()}",
            f"ZRANGE {key} {index()} {index()} WITHSCORES",
            f"ZREVRANGE {key} {index()} {index()}",
            f"ZRANGEBYSCORE {key} -inf +inf WITHSCORES",
            f"ZRANGEBYSCORE {key} ({score()} {score()} "
            f"LIMIT {random.randint(0, 5)} {random.randint(-1, 20)}",
            f"ZCOUNT {key} {score()} {score()}",
            f"ZRANGEBYLEX {key} [{member()} [{member()}",
            f"ZLEXCOUNT {key} - +",
            f"ZRANGE {key} + - BYLEX REV LIMIT 0 10",
            f"HSET {key} {fields}",
            f"HDEL {key} {many}",
            f"HGET {key} {member()}",
            f"HLEN {key}",
            f"HGETALL {key}",
            f"HINCRBY {key} {member()} {random.randint(-5, 5)}",
            f"HMGET {key} {many}",
            f"DEL {key}",
            f"TYPE {key}",
            f"EXISTS {key} {other_key}",
            f"RENAME {key} {other_key}",
            f"EXPIRE {key} 1000",
            f"PERSIST {key}",
            f"TTL {key}",
            "DBSIZE",
        ]
        return random.choice(choices)

    return [ops() for _ in range(count)]


class Connection:
    """A minimal RESP2/RESP3 client that decodes replies to Python values."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buffer = b""

    def _line(self):
        while b"\r\n" not in self.buffer:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("connection closed")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def _bytes(self, size):
        while len(self.buffer) < size + 2:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("connection closed")
            self.buffer += data
        data, self.buffer = self.buffer[:size], self.buffer[size + 2 :]
        return data

    def reply(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind in (b"+", b"-", b":", b",", b"#", b"_", b"("):
            return (kind + rest).decode(errors="replace")
        if kind in (b"$", b"="):
            size = int(rest)
            return None if size < 0 else self._bytes(size)
        if kind in (b"*", b"~", b">"):
            size = int(rest)
            if size < 0:
                return None
            items = [self.reply() for _ in range(size)]
            return ("set", sorted(map(repr, items))) if kind == b"~" else items
        if kind == b"%":
            pairs = [(self.reply(), self.reply()) for _ in range(int(rest))]
            return ("map", sorted(map(repr, pairs)))
        if kind == b"|":
            for _ in range(int(rest)):
                self.reply()
                self.reply()
            return self.reply()
        raise ValueError(f"unknown reply type {line!r}")

    def call(self, args):
        payload = b"*%d\r\n" % len(args)
        for arg in args:
            data = arg.encode()
            payload += b"$%d\r\n%s\r\n" % (len(data), data)
        self.sock.sendall(payload)
        return self.reply()


UNORDERED = {"SMEMBERS", "SINTER", "SUNION", "SDIFF", "HKEYS", "HVALS"}


def by_value(value):
    """Turns non-integer numbers into floats, recursively."""
    if isinstance(value, bytes) and (b"." in value or b"e" in value):
        try:
            return ("float", float(value))
        except ValueError:
            return value
    if isinstance(value, str) and value.startswith(","):
        try:
            return ("float", float(value[1:]))
        except ValueError:
            return value
    if isinstance(value, list):
        return [by_value(item) for item in value]
    return value


def normalize(command, value, floats_by_value):
    if floats_by_value:
        value = by_value(value)
    if isinstance(value, list):
        if command in UNORDERED:
            return sorted(map(repr, value))
        if command == "HGETALL":
            return sorted(map(repr, zip(value[::2], value[1::2])))
    return value


def start(arguments, port):
    process = subprocess.Popen(
        arguments, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    for _ in range(100):
        try:
            if Connection(port).call(["PING"]) == "+PONG":
                return process
        except OSError:
            pass
        time.sleep(0.1)
    process.kill()
    sys.exit(f"compare_with_redis.py: {arguments[0]} did not start")


def run(
    name, lines, reference_port, test_port, protocol, floats_by_value, limit
):
    reference = Connection(reference_port)
    test = Connection(test_port)
    for connection in (reference, test):
        connection.call(["FLUSHDB"])
        if protocol == 3:
            connection.call(["HELLO", "3"])
    differences = 0
    for number, line in enumerate(lines, 1):
        args = shlex.split(line)
        command = args[0].upper()
        expected = normalize(command, reference.call(args), floats_by_value)
        actual = normalize(command, test.call(args), floats_by_value)
        if expected != actual:
            differences += 1
            if differences <= limit:
                print(
                    f"{name} RESP{protocol} #{number}: {line}\n"
                    f"  redis:  {expected!r}\n  simple: {actual!r}"
                )
    return differences


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        epilog="Known differences, such as SELECT 1 with one database, are "
        "listed in the redis-differential-check skill.",
    )
    parser.add_argument("binary", help="the redis_simple binary")
    parser.add_argument("--redis-server", default="redis-server")
    parser.add_argument(
        "--port",
        type=int,
        default=8700,
        help="first of the two ports (default 8700)",
    )
    parser.add_argument(
        "--seeds", type=int, default=3, help="random runs (default 3)"
    )
    parser.add_argument(
        "--operations",
        type=int,
        default=5000,
        help="operations per random run (default 5000)",
    )
    parser.add_argument("--io-threads", type=int, default=1)
    parser.add_argument(
        "--show",
        type=int,
        default=10,
        help="differences to print per run (default 10)",
    )
    args = parser.parse_args()

    version = subprocess.run(
        [args.redis_server, "--version"], capture_output=True, text=True
    ).stdout
    major = int(version.split("v=")[1].split(".")[0]) if "v=" in version else 8
    reference_port, test_port = args.port, args.port + 1
    with tempfile.TemporaryDirectory() as directory:
        # Separate directories, so SAVE in each writes its own dump.rdb.
        reference_directory = os.path.join(directory, "redis")
        test_directory = os.path.join(directory, "simple")
        os.mkdir(reference_directory)
        os.mkdir(test_directory)
        reference = start(
            [
                args.redis_server,
                "--port",
                str(reference_port),
                "--save",
                "",
                "--appendonly",
                "no",
                "--dir",
                reference_directory,
            ],
            reference_port,
        )
        test = start(
            [
                args.binary,
                "--port",
                str(test_port),
                "--save",
                "",
                "--io-threads",
                str(args.io_threads),
                "--dir",
                test_directory,
            ],
            test_port,
        )
        try:
            runs = [("edge-cases", edge_cases())]
            runs += [
                (f"random-{seed}", random_operations(seed, args.operations))
                for seed in range(1, args.seeds + 1)
            ]
            total = 0
            for name, lines in runs:
                for protocol in (2, 3):
                    count = run(
                        name,
                        lines,
                        reference_port,
                        test_port,
                        protocol,
                        major < 8,
                        args.show,
                    )
                    print(
                        f"{name} RESP{protocol}: {len(lines)} commands, "
                        f"{count} differences"
                    )
                    total += count
        finally:
            for process in (reference, test):
                process.terminate()
                process.wait()
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
