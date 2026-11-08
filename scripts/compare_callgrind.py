#!/usr/bin/env python3
"""Shows which functions account for the difference between two profiles.

Compares the per-function instruction counts of two callgrind files, such as
the baseline-<workload>.cg and binary-<workload>.cg files profile_server.sh
writes to PROFILE_OUTPUT_DIR, and lists the functions whose counts changed
most. A function that grows while its caller shrinks by a similar amount was
usually inlined before and is now called.

Usage: scripts/compare_callgrind.py [-n COUNT] BASELINE.cg BINARY.cg
"""

import argparse
import re
import subprocess
import sys

_LINE = re.compile(r"\s*([\d,]+) \([^)]*\)\s+(.*)$")


def function_costs(path):
    """Returns {function: instructions}, summed over the binary's objects."""
    try:
        output = subprocess.run(
            ["callgrind_annotate", "--threshold=100", "--inclusive=no", path],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except FileNotFoundError:
        sys.exit("compare_callgrind.py: callgrind_annotate is required")
    except subprocess.CalledProcessError as error:
        sys.exit(f"compare_callgrind.py: {path}: {error.stderr.strip()}")
    costs = {}
    for line in output.splitlines():
        match = _LINE.match(line)
        if match is None or "PROGRAM TOTALS" in line:
            continue
        name = match.group(2)
        # Drop the "file:" prefix and the "[object]" suffix, keeping the
        # function, so the same function matches across two binaries.
        name = re.sub(r"\s+\[[^\]]*\]$", "", name)
        name = name.split(":", 1)[1] if ":" in name.split("(", 1)[0] else name
        costs[name] = costs.get(name, 0) + int(match.group(1).replace(",", ""))
    return costs


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("baseline")
    parser.add_argument("binary")
    parser.add_argument(
        "-n",
        "--count",
        type=int,
        default=15,
        help="functions to list (default 15)",
    )
    args = parser.parse_args()

    baseline = function_costs(args.baseline)
    binary = function_costs(args.binary)
    baseline_total = sum(baseline.values())
    binary_total = sum(binary.values())
    changes = sorted(
        (
            (binary.get(name, 0) - baseline.get(name, 0), name)
            for name in baseline.keys() | binary.keys()
        ),
        key=lambda change: -abs(change[0]),
    )
    print(
        f"total: {baseline_total:,} -> {binary_total:,} "
        f"({binary_total - baseline_total:+,})"
    )
    print(f"{'change':>14}  {'baseline':>14}  {'binary':>14}  function")
    for difference, name in changes[: args.count]:
        if difference == 0:
            break
        shown = name if len(name) <= 100 else name[:97] + "..."
        print(
            f"{difference:>+14,}  {baseline.get(name, 0):>14,}  "
            f"{binary.get(name, 0):>14,}  {shown}"
        )


if __name__ == "__main__":
    main()
