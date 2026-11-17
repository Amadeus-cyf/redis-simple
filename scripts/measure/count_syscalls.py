#!/usr/bin/env python3
"""Counts the server's system calls per request and per connection.

profile_server.sh counts instructions in the server's own code, and its
clients keep their connections open, so it sees neither the cost of system
calls nor the work of accepting and closing a connection. This script
attaches strace to a running server, drives it with redis-benchmark, and
prints how many of each system call the server made per operation:

  connect      a new connection for every PING (redis-benchmark -k 0), so
               the numbers are per connection: accept, setup, and close
  unpipelined  SET and GET, one request per round trip
  pipelined    SET and GET, 16 requests per round trip
  io-threads   unpipelined, with --io-threads 4 (redis_simple and Redis
               both take that option)

Pass any number of binaries, including redis-server, to compare them:

  scripts/measure/count_syscalls.py build/release/redis_simple \\
      path/to/baseline/redis_simple redis-server

The counts are per operation, but not all are the request path's own:

  * `connect` runs 10 clients (-c 10). How many connections the server
    finds waiting at once, and so its accept count, depends on that number.
  * With I/O threads, read and write include the calls that wake a thread
    (on a pipe in redis_simple, an eventfd in Redis), not only socket I/O.
  * strace slows the server, which changes counts that depend on timing,
    such as epoll_wait and futex. Compare binaries, not absolute numbers.

Linux only. Needs strace and redis-benchmark on PATH, and permission to
attach to a process that is not strace's child: kernel.yama.ptrace_scope 0,
or root (CAP_SYS_PTRACE).
"""

import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

WORKLOADS = {
    "connect": (["-t", "ping_inline", "-k", "0"], 1, []),
    "unpipelined": (["-t", "set,get"], 2, []),
    "pipelined": (["-t", "set,get", "-P", "16"], 2, []),
    "io-threads": (["-t", "set,get"], 2, ["--io-threads", "4"]),
}


def wait_until_ready(port, process):
    for _ in range(100):
        if process.poll() is not None:
            break
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as s:
                s.sendall(b"PING\r\n")
                if s.recv(64).startswith(b"+PONG"):
                    return
        except OSError:
            pass
        time.sleep(0.05)
    sys.exit("count_syscalls.py: the server did not start")


def parse_summary(path):
    """Returns {system call: calls} from strace -c output."""
    counts = {}
    with open(path) as summary:
        for line in summary:
            fields = line.split()
            if len(fields) < 5 or not fields[3].isdigit():
                continue
            name = fields[-1]
            if name != "total":
                counts[name] = int(fields[3])
    return counts


def measure(binary, port, workload, requests):
    benchmark_options, operations_per_request, server_options = WORKLOADS[workload]
    directory = tempfile.mkdtemp(prefix="count_syscalls.")
    arguments = [binary, "--port", str(port), "--dir", directory, "--save", ""]
    arguments += ["--appendonly", "no"] + server_options
    server = subprocess.Popen(
        arguments, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    summary = os.path.join(directory, "strace.txt")
    try:
        wait_until_ready(port, server)
        # Attach after startup, so loading and setup are not counted. -f
        # follows every thread, including I/O threads.
        tracer = subprocess.Popen(
            ["strace", "-f", "-c", "-o", summary, "-p", str(server.pid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.5)
        if tracer.poll() is not None:
            sys.exit(
                "count_syscalls.py: strace could not attach: "
                + tracer.stderr.read().decode().strip()
            )
        subprocess.run(
            ["redis-benchmark", "-p", str(port), "-n", str(requests), "-c", "10", "-q"]
            + benchmark_options,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        tracer.send_signal(signal.SIGINT)
        tracer.wait(timeout=60)
        counts = parse_summary(summary)
    finally:
        server.terminate()
        try:
            server.wait(timeout=60)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        shutil.rmtree(directory, ignore_errors=True)
    operations = requests * operations_per_request
    return {name: calls / operations for name, calls in counts.items()}


def main():
    parser = argparse.ArgumentParser(
        description="Count system calls per operation for one or more servers."
    )
    parser.add_argument("binaries", nargs="+", help="servers to compare")
    parser.add_argument(
        "-w",
        "--workloads",
        default=",".join(WORKLOADS),
        help="comma-separated workloads (default: all)",
    )
    parser.add_argument(
        "-n",
        "--requests",
        type=int,
        default=20000,
        help="requests per benchmark test",
    )
    parser.add_argument(
        "--min",
        type=float,
        default=0.01,
        help="hide system calls made fewer times per operation",
    )
    parser.add_argument("--port", type=int, default=8700)
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        sys.exit("count_syscalls.py: needs Linux and strace")
    for tool in ("strace", "redis-benchmark"):
        if shutil.which(tool) is None:
            sys.exit(f"count_syscalls.py: {tool} is not on PATH")
    workloads = args.workloads.split(",")
    for workload in workloads:
        if workload not in WORKLOADS:
            sys.exit(f"count_syscalls.py: unknown workload {workload}")

    labels = [os.path.basename(binary) for binary in args.binaries]
    if len(set(labels)) < len(labels):
        labels = list(args.binaries)
    for workload in workloads:
        rates = [
            measure(binary, args.port, workload, args.requests)
            for binary in args.binaries
        ]
        names = sorted(
            {
                name
                for rate in rates
                for name, value in rate.items()
                if value >= args.min
            },
            key=lambda name: -max(rate.get(name, 0) for rate in rates),
        )
        unit = "connection" if workload == "connect" else "operation"
        print(f"{workload}: system calls per {unit}")
        width = max(len(name) for name in names + ["total"])
        columns = max(12, max(len(label) for label in labels))
        print(" " * width + "".join(f" {label:>{columns}}" for label in labels))
        for name in names + ["total"]:
            values = [
                sum(rate.values()) if name == "total" else rate.get(name, 0)
                for rate in rates
            ]
            print(f"{name:<{width}}" + "".join(f" {v:>{columns}.2f}" for v in values))
        print(flush=True)


if __name__ == "__main__":
    main()
