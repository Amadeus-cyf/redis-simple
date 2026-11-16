#!/usr/bin/env python3
"""Measures how long the server stalls other clients, and how fast it expires.

Instruction counts and memory per key miss work that runs between requests:
freeing a large value, flushing the keyspace, forking for a snapshot, and
removing expired keys. For each scenario this script loads data into a fresh
server, then runs a client that sends PING in a loop while the scenario's
trigger runs on another connection, and reports:

  trigger   how long the trigger command took to answer; for `expire`, the
            time from the last key's deadline until DBSIZE reached 0
  max, p99  the longest and the 99th-percentile PING round trip
  >1ms      how many PINGs took longer than 1 ms
  ping/s    PINGs answered per second
  window s  the seconds those four columns cover: from the trigger (for
            `expire`, from the first key's deadline) until 0.5 s after it
            finished, because frees and expiry continue in steps. Rows
            with windows of different lengths have `>1ms` counts that do
            not compare directly.

The PING loop keeps the server busy, so `expire` shows expiry under load,
not on an idle server.

Timings vary from run to run on a shared machine: compare large effects
(several times longer, pauses of tens of milliseconds), and repeat a run
before reporting a small one.

Pass any number of binaries, including redis-server, to compare them:

  scripts/measure_pauses.py build/release/redis_simple \\
      path/to/baseline/redis_simple redis-server -s expire,unlink-hash

Needs redis-cli on PATH, for loading.
"""

import argparse
import math
import multiprocessing
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SCENARIOS = {
    "expire": "N strings with a TTL; time until all are removed",
    "del-hash": "DEL of one hash of N fields",
    "unlink-hash": "UNLINK of one hash of N fields",
    "unlink-set": "UNLINK of one set of N members",
    "unlink-zset": "UNLINK of one sorted set of N members",
    "unlink-list": "UNLINK of one list of N elements",
    "flushdb": "FLUSHDB of N strings",
    "bgsave": "BGSAVE of N 100-byte strings, until the child exits",
    "bgrewriteaof": "BGREWRITEAOF of N 100-byte strings, until it exits",
}


def encode(*args):
    out = [b"*%d\r\n" % len(args)]
    for arg in args:
        data = arg.encode() if isinstance(arg, str) else arg
        out.append(b"$%d\r\n%s\r\n" % (len(data), data))
    return b"".join(out)


class ReplyError(Exception):
    """An error reply from the server."""


class Connection:
    """A blocking RESP client that returns replies as raw values.

    An error reply raises ReplyError.
    """

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=120)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""

    def _receive(self):
        data = self.sock.recv(65536)
        if not data:
            raise EOFError("connection closed")
        self.buffer += data

    def _line(self):
        while b"\r\n" not in self.buffer:
            self._receive()
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def reply(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"-":
            raise ReplyError(rest.decode())
        if kind in (b"+", b":"):
            return rest
        if kind == b"$":
            size = int(rest)
            if size < 0:
                return None
            while len(self.buffer) < size + 2:
                self._receive()
            data, self.buffer = self.buffer[:size], self.buffer[size + 2 :]
            return data
        if kind == b"*":
            return [self.reply() for _ in range(int(rest))]
        raise ValueError(f"unexpected reply {line!r}")

    def call(self, *args):
        self.sock.sendall(encode(*args))
        return self.reply()


def require_free_port(port):
    """Exits if something listens on PORT: it would be measured instead."""
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
    except OSError:
        return
    sys.exit(f"{os.path.basename(sys.argv[0])}: port {port} is in use")


def commands(scenario, n, ttl_ms):
    batch = 100
    if scenario == "expire":
        for i in range(n):
            yield encode("SET", f"key:{i:012d}", "value", "PX", str(ttl_ms))
    elif scenario == "flushdb":
        for i in range(n):
            yield encode("SET", f"key:{i:012d}", "value")
    elif scenario in ("bgsave", "bgrewriteaof"):
        value = "v" * 100
        for i in range(n):
            yield encode("SET", f"key:{i:012d}", value)
    elif scenario.endswith("-hash"):
        for i in range(0, n, batch):
            pairs = []
            for j in range(i, min(i + batch, n)):
                pairs += [f"field:{j:012d}", "value"]
            yield encode("HSET", "big", *pairs)
    elif scenario == "unlink-set":
        for i in range(0, n, batch):
            members = [f"member:{j:012d}" for j in range(i, min(i + batch, n))]
            yield encode("SADD", "big", *members)
    elif scenario == "unlink-zset":
        for i in range(0, n, batch):
            pairs = []
            for j in range(i, min(i + batch, n)):
                pairs += [str(j), f"member:{j:012d}"]
            yield encode("ZADD", "big", *pairs)
    elif scenario == "unlink-list":
        for i in range(0, n, batch):
            elements = [f"element:{j:012d}" for j in range(i, min(i + batch, n))]
            yield encode("RPUSH", "big", *elements)


def load(port, data):
    """Sends DATA with redis-cli --pipe; exits if any command failed."""
    result = subprocess.run(
        ["redis-cli", "-p", str(port), "--pipe"],
        input=b"".join(data),
        stdout=subprocess.PIPE,
        check=True,
    )
    if b"errors: 0," not in result.stdout:
        summary = result.stdout.decode(errors="replace").strip()
        sys.exit(f"{os.path.basename(sys.argv[0])}: loading failed: {summary}")


def wait_for(control, field, timeout):
    """Waits until INFO persistence no longer reports FIELD:1."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if field + b":1" not in control.call("INFO", "persistence"):
            return
        time.sleep(0.02)
    sys.exit(f"measure_pauses.py: {field.decode()} did not finish")


def probe(port, stop, results):
    """Sends PING until STOP is set, then sends (start, round trip) pairs.

    It runs in its own process, so that it and the client timing the
    trigger do not wait for each other's turn at Python's interpreter lock.
    """
    connection = Connection(port)
    samples = []
    while not stop.is_set():
        start = time.monotonic()
        before = time.perf_counter()
        connection.call("PING")
        samples.append((start, time.perf_counter() - before))
    results.send(samples)


def run(binary, port, scenario, n, ttl_ms):
    require_free_port(port)
    directory = tempfile.mkdtemp(prefix="measure_pauses.")
    arguments = [binary, "--port", str(port), "--dir", directory, "--save", ""]
    arguments += ["--appendonly", "yes" if scenario == "bgrewriteaof" else "no"]
    # Loading must not start a rewrite of its own.
    arguments += ["--auto-aof-rewrite-percentage", "0"]
    server = subprocess.Popen(
        arguments, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    try:
        for _ in range(100):
            if server.poll() is not None:
                sys.exit(f"measure_pauses.py: {binary} exited at startup")
            try:
                Connection(port).call("PING")
                break
            except OSError:
                time.sleep(0.05)
        else:
            sys.exit(f"measure_pauses.py: {binary} did not start")
        control = Connection(port)
        load_start = time.monotonic()
        load(port, commands(scenario, n, ttl_ms))
        load_end = time.monotonic()
        if scenario == "expire" and load_end - load_start > ttl_ms / 1000:
            sys.exit(
                "measure_pauses.py: loading took longer than the TTL; " "raise --ttl-ms"
            )

        if scenario == "expire":
            # Every key's deadline lies between these two moments.
            first_deadline = load_start + ttl_ms / 1000
            last_deadline = load_end + ttl_ms / 1000
            # Leave the prober the half second below to start in.
            time.sleep(max(0.0, first_deadline - 0.5 - time.monotonic()))
        context = multiprocessing.get_context("spawn")
        stop = context.Event()
        receiver, sender = context.Pipe(duplex=False)
        prober = context.Process(target=probe, args=(port, stop, sender))
        prober.start()
        # With only the prober's end open, recv() fails if the prober dies.
        sender.close()
        time.sleep(0.5)
        window_start = time.monotonic()
        if scenario == "expire":
            deadline = last_deadline + 300
            while int(control.call("DBSIZE")) > 0:
                if time.monotonic() > deadline:
                    sys.exit("measure_pauses.py: keys did not expire")
                time.sleep(0.01)
            trigger_ms = (time.monotonic() - last_deadline) * 1000
        else:
            command = {
                "del-hash": ("DEL", "big"),
                "flushdb": ("FLUSHDB",),
                "bgsave": ("BGSAVE",),
                "bgrewriteaof": ("BGREWRITEAOF",),
            }.get(scenario, ("UNLINK", "big"))
            start = time.perf_counter()
            try:
                control.call(*command)
            except ReplyError as error:
                sys.exit(f"measure_pauses.py: {command[0]}: {error}")
            trigger_ms = (time.perf_counter() - start) * 1000
            if scenario == "bgsave":
                wait_for(control, b"rdb_bgsave_in_progress", 300)
            elif scenario == "bgrewriteaof":
                wait_for(control, b"aof_rewrite_in_progress", 300)
        # Stepwise frees and expiry continue after the command answers.
        time.sleep(0.5)
        window_end = time.monotonic()
        stop.set()
        samples = receiver.recv()
        prober.join()
    finally:
        server.terminate()
        try:
            server.wait(timeout=60)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        shutil.rmtree(directory, ignore_errors=True)

    window = window_end - window_start
    waits = sorted(w for start, w in samples if window_start <= start < window_end)
    if not waits:
        return trigger_ms, 0.0, 0.0, 0, 0.0, window
    p99 = waits[math.ceil(len(waits) * 0.99) - 1]
    slow = sum(1 for w in waits if w > 0.001)
    return trigger_ms, waits[-1] * 1000, p99 * 1000, slow, len(waits) / window, window


def main():
    parser = argparse.ArgumentParser(
        description="Measure pauses and expiry speed of one or more servers.",
        epilog="Scenarios: "
        + "; ".join(f"{name}: {text}" for name, text in SCENARIOS.items()),
    )
    parser.add_argument("binaries", nargs="+", help="servers to compare")
    parser.add_argument(
        "-s",
        "--scenarios",
        default=",".join(SCENARIOS),
        help="comma-separated scenarios (default: all)",
    )
    parser.add_argument("-n", "--keys", type=int, default=1_000_000)
    parser.add_argument(
        "--ttl-ms",
        type=int,
        default=10_000,
        help="TTL of the expire scenario's keys; must exceed loading time",
    )
    parser.add_argument("--port", type=int, default=8900)
    args = parser.parse_args()
    if shutil.which("redis-cli") is None:
        sys.exit("measure_pauses.py: redis-cli is not on PATH")
    scenarios = args.scenarios.split(",")
    for scenario in scenarios:
        if scenario not in SCENARIOS:
            sys.exit(f"measure_pauses.py: unknown scenario {scenario}")

    labels = [os.path.basename(binary) for binary in args.binaries]
    if len(set(labels)) < len(labels):
        labels = list(args.binaries)
    width = max(len(label) for label in labels)
    print(
        f"{'scenario':<14} {'binary':<{width}} {'trigger ms':>10} "
        f"{'max ms':>8} {'p99 ms':>8} {'>1ms':>6} {'ping/s':>8} {'window s':>8}"
    )
    for scenario in scenarios:
        for label, binary in zip(labels, args.binaries):
            trigger, longest, p99, slow, rate, window = run(
                binary, args.port, scenario, args.keys, args.ttl_ms
            )
            print(
                f"{scenario:<14} {label:<{width}} {trigger:>10.1f} "
                f"{longest:>8.1f} {p99:>8.3f} {slow:>6} {rate:>8.0f} "
                f"{window:>8.1f}",
                flush=True,
            )


if __name__ == "__main__":
    main()
