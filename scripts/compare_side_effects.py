#!/usr/bin/env python3
"""Compares redis_simple's side effects with a reference redis-server.

compare_with_redis.py compares replies. Two commands can reply alike and
still differ in what they change, so for each case this script also
compares:

  * the change count, from INFO's rdb_changes_since_last_save, which decides
    whether a command is written to the AOF and counted toward save points;
  * whether a WATCH on the keys, held by a second client, is failed.

Each case starts from the same small keyspace. Exits nonzero and prints the
cases that differ.

Usage:
  scripts/compare_side_effects.py build/debug/redis_simple
      [--redis-server PATH]
"""

import argparse
import os
import shlex
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_with_redis import Connection, start  # noqa: E402

SETUP = [
    "FLUSHDB",
    "SET s hello",
    "SET n 10",
    "RPUSH l a b c",
    "SADD st x y z",
    "SADD ist 1 2 3",
    "ZADD z 1 a 2 b",
    "HSET h f v g 5",
    "SET t v EX 1000",
]
WATCHED = "WATCH s n l st ist z h t missing missing2"

# Mostly commands that change nothing, or less than they could: those are
# where the change count and WATCH differ from the reply. Add a case with
# every new write command and every new option of one.
CASES = """
LTRIM missing 0 -1
LTRIM l 0 -1
LTRIM l 0 0
LTRIM l 5 1
LREM l 0 nope
LREM l 0 a
LREM missing 0 a
LSET l 0 a
LSET l 0 zz
LSET l 10 zz
LPOP l
LPOP l 0
LPOP l 10
LPOP missing
RPOP l 2
LPUSH l x y
RPUSH missing x y z
SADD st x
SADD st x new
SADD ist 1
SADD ist 9 a
SREM st nope
SREM st x nope
SREM st x y z
SREM missing x
ZADD z 1 a
ZADD z 5 a
ZADD z XX 5 nope
ZADD missing XX 5 nope
ZADD z NX 5 a
ZADD z GT 0 a
ZADD z CH 1 a 9 b 3 c
ZADD z INCR 0 a
ZADD z INCR 2 a
ZADD z NX INCR 1 a
ZREM z nope
ZREM z a nope
ZREM z a b
HSET h f v
HSET h f w new 1
HMSET h f v
HDEL h nope
HDEL h f nope
HDEL h f g
HDEL missing f
HINCRBY h g 0
HINCRBY h g 5
HINCRBY h f 1
HINCRBY missing f 3
HINCRBY missing f abc
SET s hello
SET s other NX
SET missing v XX
SET s v GET
SET missing v NX GET
SET t v KEEPTTL
APPEND s ""
APPEND missing ""
APPEND s x
INCR n
INCRBY n 0
INCR s
MSET a 1 b 2 s 3
DEL missing
DEL s missing n
UNLINK s missing n
EXPIRE s 100
EXPIRE s 100 XX
EXPIRE t 100 GT
EXPIRE t 100 LT
EXPIRE t 100 NX
EXPIRE missing 100
EXPIRE s -1
EXPIRE s -9223372036854775808
PEXPIRE s -9223372036854775808
EXPIREAT s 1
PEXPIREAT s -1
PERSIST s
PERSIST t
PERSIST missing
RENAME s s
RENAME s s2
RENAME s n
RENAME t s
RENAME missing x
FLUSHDB
FLUSHDB;FLUSHDB
GET s
MULTI;SET s 1;INCR s;LPUSH s x;EXEC
MULTI;GET s;EXEC
MULTI;SET s 1;DISCARD
""".strip().splitlines()


def changes(connection):
    info = connection.call(["INFO", "persistence"])
    text = info.decode() if isinstance(info, bytes) else info
    for line in text.splitlines():
        if line.startswith("rdb_changes_since_last_save:"):
            return int(line.split(":")[1])
    sys.exit("compare_side_effects.py: no rdb_changes_since_last_save")


def run_case(port, case):
    """Returns (replies, change count, whether the WATCH was failed)."""
    client, watcher = Connection(port), Connection(port)
    for line in SETUP:
        client.call(shlex.split(line))
    watcher.call(shlex.split(WATCHED))
    before = changes(client)
    replies = [client.call(shlex.split(line)) for line in case.split(";")]
    after = changes(client)
    watcher.call(["MULTI"])
    watcher.call(["PING"])
    failed = watcher.call(["EXEC"]) is None
    return replies, after - before, failed


def main():
    parser = argparse.ArgumentParser(
        description="Compare redis_simple's change counts and WATCH "
        "behavior with a reference redis-server."
    )
    parser.add_argument("binary", help="the redis_simple binary")
    parser.add_argument("--redis-server", default="redis-server")
    parser.add_argument("--reference-port", type=int, default=6390)
    parser.add_argument("--test-port", type=int, default=8090)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        servers = []
        for name, binary, port in (
            ("redis", args.redis_server, args.reference_port),
            ("simple", args.binary, args.test_port),
        ):
            server_directory = os.path.join(directory, name)
            os.mkdir(server_directory)
            arguments = [binary, "--port", str(port), "--dir", server_directory]
            servers.append(start(arguments, port))
        try:
            differences = 0
            for case in CASES:
                reference = run_case(args.reference_port, case)
                test = run_case(args.test_port, case)
                if reference != test:
                    differences += 1
                    print(f"{case}")
                    print(f"  redis:  {reference}")
                    print(f"  simple: {test}")
            print(f"{len(CASES)} cases, {differences} differences")
            print("columns: replies, change count, WATCH failed")
        finally:
            for server in servers:
                server.terminate()
                server.wait()
    sys.exit(1 if differences else 0)


if __name__ == "__main__":
    main()
