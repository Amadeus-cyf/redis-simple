#!/usr/bin/env python3
"""Compares two server builds end to end: throughput, latency, CPU, memory.

The other measurement scripts each answer one question precisely. This one
gives the overall picture of a change, or of a batch of changes, as two
Markdown tables that paste into a pull request:

  Latency, throughput, server CPU
      redis-benchmark requests per second and p50/p99 latency, and the user
      plus system CPU seconds the server spent on the workload, for ten
      workloads: with and without pipelining, collection commands, 64 KiB
      values, AOF with `always` and `everysec`, and four I/O threads.
  Memory
      Growth in the server's memory after loading one million strings (3-, 36-, and 100-byte
      values, and with a TTL), a sorted set of one million members, 200,000
      small hashes, sorted sets, sets, and lists, and 3,000 idle
      connections. On Linux this is resident memory. On macOS it is the
      physical footprint (what Activity Monitor shows), because resident
      memory there keeps counting pages the allocator has returned until
      the kernel reclaims them.

Every measurement starts a fresh server in an empty directory with saving
off, and runs alternate between the two builds so that drift on the machine
affects both. Throughput and latency report the median of the runs; server
CPU reports the minimum, which a busy machine disturbs least. Throughput
and latency still vary by a few percent between runs: trust the CPU column
for small differences, and repeat a run before reporting one.

  scripts/compare_builds.py path/to/baseline/redis_simple \\
      build/release/redis_simple --runs 5
  scripts/compare_builds.py BASELINE BINARY --only memory --case "sorted set"

Works on macOS and Linux. Needs redis-benchmark on PATH, and on macOS
vmmap, which ships with the Xcode command line tools.
"""

import argparse
import csv
import io
import os
import re
import resource
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time

AOF_ALWAYS = ["--appendonly", "yes", "--appendfsync", "always"]
AOF_EVERYSEC = ["--appendonly", "yes", "--appendfsync", "everysec"]
IO_THREADS = ["--io-threads", "4"]

# (label, server arguments, redis-benchmark arguments)
LATENCY_WORKLOADS = [
    ("no pipeline", [],
     ["-t", "set,get", "-n", "400000", "-r", "1000000", "-c", "50"]),
    ("pipeline 16", [],
     ["-t", "set,get", "-n", "2000000", "-r", "1000000", "-P", "16",
      "-c", "50"]),
    ("collections, pipeline 16", [],
     ["-t", "lpush,lrange_100,zadd,hset,sadd", "-n", "400000",
      "-r", "100000", "-P", "16", "-c", "50"]),
    ("64 KiB values, no pipeline", [],
     ["-t", "set,get", "-d", "65536", "-n", "100000", "-c", "50"]),
    ("AOF always, no pipeline", AOF_ALWAYS,
     ["-t", "set", "-n", "100000", "-r", "1000000", "-c", "50"]),
    ("AOF always, pipeline 16", AOF_ALWAYS,
     ["-t", "set", "-n", "400000", "-r", "1000000", "-P", "16", "-c", "50"]),
    ("AOF everysec, no pipeline", AOF_EVERYSEC,
     ["-t", "set", "-n", "400000", "-r", "1000000", "-c", "50"]),
    ("AOF everysec, pipeline 16", AOF_EVERYSEC,
     ["-t", "set", "-n", "2000000", "-r", "1000000", "-P", "16",
      "-c", "50"]),
    ("4 I/O threads, no pipeline", IO_THREADS,
     ["-t", "set,get", "-n", "400000", "-r", "1000000", "-c", "50"]),
    ("4 I/O threads, pipeline 16", IO_THREADS,
     ["-t", "set,get", "-n", "2000000", "-r", "1000000", "-P", "16",
      "-c", "50"]),
]

KIB_PER_UNIT = {"K": 1, "M": 1024, "G": 1024 * 1024}
IDLE_CONNECTIONS = 3000
LARGE_VALUE_BYTES = 48 * 1024
# Enough descriptors for the idle connections on both ends of each socket.
WANTED_OPEN_FILES = 16384


class Server:
    """A server process in its own empty directory."""

    def __init__(self, binary, port, arguments=()):
        self.port = port
        self.directory = tempfile.mkdtemp()
        self.process = subprocess.Popen(
            [binary, "--port", str(port), "--dir", self.directory,
             "--save", "", *arguments],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(200):
            try:
                with socket.create_connection(("127.0.0.1", port), 0.2) as s:
                    s.sendall(b"PING\r\n")
                    if s.recv(16).startswith(b"+PONG"):
                        return
            except OSError:
                pass
            time.sleep(0.05)
        self.stop()
        raise RuntimeError(f"{binary} did not start on port {port}")

    def cpu_seconds(self):
        """User plus system CPU seconds the server has used so far."""
        stat = f"/proc/{self.process.pid}/stat"
        if os.path.exists(stat):
            # The command name, field 2, may hold spaces; count from its
            # closing parenthesis. utime and stime are fields 14 and 15.
            with open(stat) as file:
                fields = file.read().rsplit(") ", 1)[1].split()
            ticks = int(fields[11]) + int(fields[12])
            return ticks / os.sysconf("SC_CLK_TCK")
        # macOS has no /proc; its ps prints [HH:]MM:SS.cc.
        out = subprocess.check_output(
            ["ps", "-o", "time=", "-p", str(self.process.pid)], text=True)
        seconds = 0.0
        for part in out.strip().split(":"):
            seconds = seconds * 60 + float(part)
        return seconds

    def memory_kib(self):
        """The memory the server holds: see the module docstring."""
        pid = str(self.process.pid)
        if sys.platform != "darwin":
            return int(subprocess.check_output(
                ["ps", "-o", "rss=", "-p", pid], text=True))
        out = subprocess.check_output(["vmmap", "-summary", pid], text=True,
                                      stderr=subprocess.DEVNULL)
        match = re.search(r"Physical footprint:\s+([\d.]+)([KMG])", out)
        if match is None:
            raise RuntimeError("vmmap printed no physical footprint")
        return float(match.group(1)) * KIB_PER_UNIT[match.group(2)]

    def stop(self):
        self.process.terminate()
        self.process.wait()
        shutil.rmtree(self.directory, ignore_errors=True)


def run_benchmark(port, arguments):
    """Returns {test: (requests per second, p50 ms, p99 ms)}."""
    out = subprocess.check_output(
        ["redis-benchmark", "-p", str(port), "--csv", *arguments], text=True,
        stderr=subprocess.DEVNULL)
    result = {}
    for record in csv.DictReader(io.StringIO(out)):
        # Test names carry a suffix, such as "LRANGE_100 (first 100
        # elements)"; the first word is enough to tell them apart.
        result[record["test"].split(" ")[0]] = (
            float(record["rps"]), float(record["p50_latency_ms"]),
            float(record["p99_latency_ms"]))
    return result


def send_commands(port, commands):
    """Sends commands (lists of byte strings) and waits for the last reply."""
    marker = b"done-%d" % os.getpid()
    with socket.create_connection(("127.0.0.1", port)) as connection:
        buffer = bytearray()

        def encode(command):
            buffer.extend(b"*%d\r\n" % len(command))
            for argument in command:
                buffer.extend(b"$%d\r\n%s\r\n" % (len(argument), argument))

        def discard_replies():
            connection.setblocking(False)
            try:
                while connection.recv(1 << 20):
                    pass
            except BlockingIOError:
                pass
            connection.setblocking(True)

        for command in commands:
            encode(command)
            if len(buffer) > (1 << 16):
                connection.sendall(buffer)
                buffer.clear()
                discard_replies()
        encode([b"ECHO", marker])
        connection.sendall(buffer)
        tail = b""
        while marker not in tail:
            received = connection.recv(1 << 20)
            if not received:
                raise RuntimeError("server closed the connection")
            tail = (tail + received)[-64:]


def strings(count, value_size, ttl):
    value = b"x" * value_size
    for index in range(count):
        command = [b"SET", b"key:%012d" % index, value]
        if ttl:
            command += [b"EX", b"1000000"]
        yield command


def small_collections(command, count, members, integer=False):
    for index in range(count):
        key = b"c:%09d" % index
        for member in range(members):
            name = b"%d" % member if integer else b"member-%d" % member
            if command == b"HSET":
                yield [command, key, name, b"value"]
            elif command == b"ZADD":
                yield [command, key, b"%d" % member, name]
            else:
                yield [command, key, name]


def big_sorted_set(count):
    for index in range(count):
        # 7919 is prime, so the scores arrive in a scattered order.
        score = index * 7919 % count
        yield [b"ZADD", b"z", b"%d" % score, b"m:%09d" % index]


def idle_connections(port, count, request, reply_bytes):
    """Opens connections that each send one request and read its reply.

    Returns the connections; the caller keeps them open while it measures.
    """
    connections = []
    for _ in range(count):
        connection = socket.create_connection(("127.0.0.1", port))
        connection.sendall(request)
        connections.append(connection)
    for connection in connections:
        remaining = reply_bytes
        while remaining > 0:
            received = connection.recv(1 << 16)
            if not received:
                raise RuntimeError("server closed an idle connection")
            remaining -= len(received)
    return connections


def idle_after_ping(server):
    reply = b"+PONG\r\n"
    return idle_connections(server.port, IDLE_CONNECTIONS, b"PING\r\n",
                            len(reply))


def idle_after_large_get(server):
    value = b"v" * LARGE_VALUE_BYTES
    send_commands(server.port, [[b"SET", b"big", value]])
    reply_bytes = len(b"$%d\r\n\r\n" % len(value)) + len(value)
    return idle_connections(server.port, IDLE_CONNECTIONS,
                            b"*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n", reply_bytes)


def loader(commands):
    """Returns a memory case that loads what commands() yields."""
    return lambda server: send_commands(server.port, commands())


# (label, function that takes a Server and loads it). Whatever the function
# returns stays alive until the server's memory has been read.
MEMORY_CASES = [
    ("1M strings, 3-byte value",
     loader(lambda: strings(1_000_000, 3, False))),
    ("1M strings, 36-byte value",
     loader(lambda: strings(1_000_000, 36, False))),
    ("1M strings, 100-byte value",
     loader(lambda: strings(1_000_000, 100, False))),
    ("1M strings, 36-byte value, with TTL",
     loader(lambda: strings(1_000_000, 36, True))),
    ("one sorted set, 1M members", loader(lambda: big_sorted_set(1_000_000))),
    ("200k hashes of 8 fields",
     loader(lambda: small_collections(b"HSET", 200_000, 8))),
    ("200k sorted sets of 8 members",
     loader(lambda: small_collections(b"ZADD", 200_000, 8))),
    ("200k sets of 8 integers",
     loader(lambda: small_collections(b"SADD", 200_000, 8, integer=True))),
    ("200k lists of 8 elements",
     loader(lambda: small_collections(b"RPUSH", 200_000, 8))),
    (f"{IDLE_CONNECTIONS:,} idle connections after PING", idle_after_ping),
    (f"{IDLE_CONNECTIONS:,} idle connections after a 48 KiB GET",
     idle_after_large_get),
]


def percent_change(old, new):
    return f"{(new / old - 1) * 100:+.1f}%" if old else "n/a"


def measure_latency(binaries, port, runs, workloads):
    print("## Latency, throughput, server CPU")
    print("| Workload | Command | Requests/s old | new | change | p50 ms old "
          "| new | p99 ms old | new | Server CPU s old | new | change |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- "
          "| --- |")
    for label, server_arguments, workload in workloads:
        cpu = {name: [] for name in binaries}
        tests = {name: {} for name in binaries}
        for _ in range(runs):
            for name, binary in binaries.items():
                server = Server(binary, port, server_arguments)
                try:
                    before = server.cpu_seconds()
                    results = run_benchmark(port, workload)
                    cpu[name].append(server.cpu_seconds() - before)
                finally:
                    server.stop()
                for test, values in results.items():
                    tests[name].setdefault(test, []).append(values)
        old_cpu, new_cpu = min(cpu["old"]), min(cpu["new"])
        # The CPU time covers the whole workload, so it goes on the first
        # of the workload's rows only.
        workload_cells = (f"{old_cpu:.2f} | {new_cpu:.2f} | "
                          f"{percent_change(old_cpu, new_cpu)}")
        for test in tests["old"]:
            old, new = (
                [statistics.median(column)
                 for column in zip(*tests[name][test])]
                for name in ("old", "new"))
            print(f"| {label} | {test} | {old[0]:,.0f} | {new[0]:,.0f} | "
                  f"{percent_change(old[0], new[0])} | {old[1]:.3f} | "
                  f"{new[1]:.3f} | {old[2]:.3f} | {new[2]:.3f} | "
                  f"{workload_cells} |")
            label, workload_cells = "", " | | "
        sys.stdout.flush()


def measure_memory(binaries, port, cases, settle_seconds):
    print("\n## Memory")
    print("| Data | Old MiB | New MiB | Change |")
    print("| --- | --- | --- | --- |")
    for label, load in cases:
        used = {}
        for name, binary in binaries.items():
            server = Server(binary, port)
            try:
                idle = server.memory_kib()
                kept_alive = load(server)
                # Lets the server finish work it does between requests,
                # such as rehashing and shrinking idle clients' buffers.
                time.sleep(settle_seconds)
                used[name] = (server.memory_kib() - idle) / 1024
                del kept_alive
            finally:
                server.stop()
        print(f"| {label} | {used['old']:.1f} | {used['new']:.1f} | "
              f"{percent_change(used['old'], used['new'])} |")
        sys.stdout.flush()


def raise_open_file_limit():
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    wanted = WANTED_OPEN_FILES
    if hard != resource.RLIM_INFINITY:
        wanted = min(wanted, hard)
    if soft == resource.RLIM_INFINITY or soft >= wanted:
        return
    resource.setrlimit(resource.RLIMIT_NOFILE, (wanted, hard))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("baseline", help="the server binary to compare with")
    parser.add_argument("binary", help="the server binary under test")
    parser.add_argument("--runs", type=int, default=5,
                        help="benchmark runs per build and workload "
                             "(default 5)")
    parser.add_argument("--port", type=int, default=7450)
    parser.add_argument("--only", choices=["latency", "memory"],
                        help="print only this table")
    parser.add_argument("--case", default="",
                        help="only the rows whose label contains this text")
    parser.add_argument("--settle", type=float, default=3.0,
                        help="seconds to wait after loading before reading "
                             "memory (default 3)")
    options = parser.parse_args()
    if options.runs < 1:
        parser.error("--runs must be at least 1")
    for binary in (options.baseline, options.binary):
        if not os.access(binary, os.X_OK):
            parser.error(f"not an executable server binary: {binary}")
    if shutil.which("redis-benchmark") is None and options.only != "memory":
        parser.error("redis-benchmark is not on PATH")

    binaries = {"old": options.baseline, "new": options.binary}
    workloads = [w for w in LATENCY_WORKLOADS if options.case in w[0]]
    cases = [c for c in MEMORY_CASES if options.case in c[0]]
    raise_open_file_limit()
    if options.only != "memory" and workloads:
        measure_latency(binaries, options.port, options.runs, workloads)
    if options.only != "latency" and cases:
        measure_memory(binaries, options.port, cases, options.settle)


if __name__ == "__main__":
    main()
