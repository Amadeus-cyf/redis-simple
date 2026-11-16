#!/usr/bin/env python3
"""Measures how a snapshot child writes its file to disk.

A BGSAVE or AOF rewrite child writes the whole dataset. Redis' rioFileWrite
starts writeback as it goes and drops the written pages from the page cache,
so the save neither builds up gigabytes of dirty pages that stall the
machine's other writers at the end, nor evicts the page cache that other
processes use. Neither CPU instruction counts nor memory per key show this,
so for each scenario this script reports:

  seconds    from the command until the child is done
  file MiB   the size of the file written
  dirty MiB  the largest growth of dirty and under-writeback memory
             (/proc/meminfo Dirty + Writeback) during the save, on Linux
  cached %   how much of the finished file is still in the page cache
  sync calls (with --trace, Linux) the fsync, fdatasync, sync_file_range,
             and fadvise64 calls of the server and its child together, from
             a second run under strace

dirty MiB counts the whole machine, so run it on an otherwise quiet one.
The servers write under --dir, which must be on a disk: on tmpfs nothing is
written back and nothing is dirty. The default, the temporary directory, is
a tmpfs on some Linux systems (check with `df /tmp`); pass --dir there.

  scripts/measure/measure_snapshot_io.py build/release/redis_simple \\
      path/to/baseline/redis_simple redis-server --trace

Needs redis-cli on PATH, for loading.
"""

import argparse
import ctypes
import ctypes.util
import glob
import mmap
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_pauses import (  # noqa: E402
    Connection,
    ReplyError,
    encode,
    load,
    require_free_port,
)

SCENARIOS = {
    "bgsave": ("BGSAVE", b"rdb_bgsave_in_progress", "dump.rdb"),
    "bgrewriteaof": (
        "BGREWRITEAOF",
        b"aof_rewrite_in_progress",
        "appendonlydir/*.base.*",
    ),
}
SYNC_CALLS = ("fsync", "fdatasync", "sync_file_range", "fadvise64")


def dirty_bytes():
    """Returns Dirty + Writeback from /proc/meminfo, or None elsewhere."""
    try:
        with open("/proc/meminfo") as meminfo:
            total = 0
            for line in meminfo:
                name, value = line.split(":")
                if name in ("Dirty", "Writeback"):
                    total += int(value.split()[0]) * 1024
            return total
    except OSError:
        return None


def cached_fraction(path):
    """Returns the fraction of PATH's pages in the page cache (mincore)."""
    size = os.path.getsize(path)
    if size == 0:
        return 0.0
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_long,
    ]
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
    page = mmap.PAGESIZE
    pages = (size + page - 1) // page
    with open(path, "rb") as file:
        address = libc.mmap(
            None, size, mmap.PROT_READ, mmap.MAP_SHARED, file.fileno(), 0
        )
        if address in (None, ctypes.c_void_p(-1).value):
            return float("nan")
        try:
            vector = ctypes.create_string_buffer(pages)
            if libc.mincore(address, size, vector) != 0:
                return float("nan")
            resident = sum(byte & 1 for byte in vector.raw)
        finally:
            libc.munmap(address, size)
    return resident / pages


def start(binary, port, directory, scenario):
    require_free_port(port)
    arguments = [binary, "--port", str(port), "--dir", directory, "--save", ""]
    arguments += ["--appendonly", "yes" if scenario == "bgrewriteaof" else "no"]
    # Loading must not start a rewrite of its own.
    arguments += ["--auto-aof-rewrite-percentage", "0"]
    server = subprocess.Popen(
        arguments, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    for _ in range(100):
        if server.poll() is not None:
            sys.exit(f"measure_snapshot_io.py: {binary} exited at startup")
        try:
            Connection(port).call("PING")
            return server
        except OSError:
            time.sleep(0.05)
    server.kill()
    sys.exit(f"measure_snapshot_io.py: {binary} did not start")


def stop(server):
    server.terminate()
    try:
        server.wait(timeout=120)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()


def strings(keys, value_bytes):
    # One random hex value for every key: LZF finds little to compress in
    # it, so the file is about as large as the data.
    value = os.urandom(value_bytes).hex().encode()[:value_bytes]
    for i in range(keys):
        yield encode("SET", f"key:{i:012d}", value)


def run(binary, port, scenario, keys, value_bytes, trace, parent):
    command, in_progress, pattern = SCENARIOS[scenario]
    directory = tempfile.mkdtemp(prefix="measure_snapshot_io.", dir=parent)
    server = None
    tracer = None
    trace_output = os.path.join(directory, "strace.txt")
    try:
        server = start(binary, port, directory, scenario)
        load(port, strings(keys, value_bytes))
        control = Connection(port)
        if scenario == "bgrewriteaof":
            # Automatic rewrites are off, so none should be running.
            while in_progress + b":1" in control.call("INFO", "persistence"):
                time.sleep(0.05)
        for path in glob.glob(os.path.join(directory, pattern)):
            os.remove(path)
        os.sync()
        if trace:
            tracer = subprocess.Popen(
                [
                    "strace",
                    "-f",
                    "-c",
                    "-o",
                    trace_output,
                    "-e",
                    "trace=" + ",".join(SYNC_CALLS),
                    "-p",
                    str(server.pid),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            time.sleep(0.5)
            if tracer.poll() is not None:
                sys.exit(
                    "measure_snapshot_io.py: strace could not attach: "
                    + tracer.stderr.read().decode().strip()
                )
        baseline = dirty_bytes()
        peak = 0
        started = time.monotonic()
        try:
            control.call(command)
        except ReplyError as error:
            sys.exit(f"measure_snapshot_io.py: {command}: {error}")
        time.sleep(0.05)
        while in_progress + b":1" in control.call("INFO", "persistence"):
            if baseline is not None:
                peak = max(peak, dirty_bytes() - baseline)
            time.sleep(0.01)
        seconds = time.monotonic() - started
        files = glob.glob(os.path.join(directory, pattern))
        if not files:
            sys.exit(f"measure_snapshot_io.py: {binary} wrote no {pattern}")
        size = os.path.getsize(files[0])
        cached = cached_fraction(files[0])
        calls = {}
        if tracer is not None:
            tracer.send_signal(signal.SIGINT)
            tracer.wait(timeout=60)
            with open(trace_output) as summary:
                for line in summary:
                    fields = line.split()
                    if (
                        len(fields) >= 5
                        and fields[3].isdigit()
                        and fields[-1] != "total"
                    ):
                        calls[fields[-1]] = int(fields[3])
    finally:
        if tracer is not None and tracer.poll() is None:
            tracer.kill()
        if server is not None:
            stop(server)
        shutil.rmtree(directory, ignore_errors=True)
    dirty = None if baseline is None else peak / 2**20
    return seconds, size / 2**20, dirty, cached * 100, calls


def main():
    parser = argparse.ArgumentParser(
        description="Measure how snapshot children write to disk."
    )
    parser.add_argument("binaries", nargs="+", help="servers to compare")
    parser.add_argument(
        "-s",
        "--scenarios",
        default=",".join(SCENARIOS),
        help="comma-separated: " + ", ".join(SCENARIOS),
    )
    parser.add_argument("-n", "--keys", type=int, default=1_000_000)
    parser.add_argument("--value-bytes", type=int, default=500)
    parser.add_argument(
        "--trace",
        action="store_true",
        help="also count the server's and its child's sync calls with strace "
        "(Linux)",
    )
    parser.add_argument(
        "--dir", help="where the servers write (default: the temporary directory)"
    )
    parser.add_argument("--port", type=int, default=8800)
    args = parser.parse_args()
    if shutil.which("redis-cli") is None:
        sys.exit("measure_snapshot_io.py: redis-cli is not on PATH")
    if args.trace and shutil.which("strace") is None:
        sys.exit("measure_snapshot_io.py: --trace needs strace on PATH")
    scenarios = args.scenarios.split(",")
    for scenario in scenarios:
        if scenario not in SCENARIOS:
            sys.exit(f"measure_snapshot_io.py: unknown scenario {scenario}")

    labels = [os.path.basename(binary) for binary in args.binaries]
    if len(set(labels)) < len(labels):
        labels = list(args.binaries)
    width = max(len(label) for label in labels)
    header = (
        f"{'scenario':<13} {'binary':<{width}} {'seconds':>8} "
        f"{'file MiB':>9} {'dirty MiB':>10} {'cached %':>9}"
    )
    if args.trace:
        header += "  sync calls"
    print(header)
    for scenario in scenarios:
        for label, binary in zip(labels, args.binaries):
            seconds, size, dirty, cached, _ = run(
                binary,
                args.port,
                scenario,
                args.keys,
                args.value_bytes,
                False,
                args.dir,
            )
            line = (
                f"{scenario:<13} {label:<{width}} {seconds:>8.2f} "
                f"{size:>9.1f} "
                + (f"{dirty:>10.1f}" if dirty is not None else f"{'-':>10}")
                + f" {cached:>9.1f}"
            )
            if args.trace:
                calls = run(
                    binary,
                    args.port,
                    scenario,
                    args.keys,
                    args.value_bytes,
                    True,
                    args.dir,
                )[4]
                line += "  " + (
                    ", ".join(
                        f"{name} {count}" for name, count in sorted(calls.items())
                    )
                    or "none"
                )
            print(line, flush=True)


if __name__ == "__main__":
    main()
