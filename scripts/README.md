# Scripts

Run every script from the repository root.

- `run_*.sh`: CTest and CI runners (integration tests, leak check,
  clang-tidy, the Linux Docker check). The integration runners source
  `wait_for_server.sh`, which waits until a started server accepts
  connections (polling with `nc -z`, or one second where that is missing)
  instead of a fixed sleep, and stops a server that never does.
- `format.sh`: clang-format wrapper.
- `compare_with_redis.py`, `compare_side_effects.py`, `check_*`,
  `run_redis_compatibility_check.sh`, `build_latest_redis.sh`: comparisons
  with a real Redis.
- [`measure/`](measure/README.md): performance and memory measurement,
  not used by CTest or CI.

## Leak check

Run the platform-specific memory leak check after building the corresponding
preset:

```sh
scripts/run_leak_check.sh
```

On macOS, the script runs the debug unit-test binary under Apple's `leaks`
tool because Apple AddressSanitizer does not provide LeakSanitizer. It skips
death tests and `AofTest`, whose forked children stall `leaks`. On
Linux, it runs the complete sanitizer CTest suite with LeakSanitizer explicitly
enabled.

## Comparing with Redis

When a reference Redis server is available on port 6379, run the focused
differential protocol and command check with:

```sh
scripts/run_redis_compatibility_check.sh build/debug/redis_simple
```

The script uses `redis-cli` against both servers and covers RESP requests,
spaces in arguments, expiration replies, sorted-set range semantics, and
transaction replies.

To compare every supported command's replies, edge cases and seeded random
operations included, in RESP2 and RESP3 (the script starts both servers):

```sh
scripts/compare_with_redis.py build/debug/redis_simple
```

To compare what replies do not show, each write command's change count,
whether it fails another client's `WATCH`, and the keyspace it leaves
behind (so a failed command that leaves an empty key shows up):

```sh
scripts/compare_side_effects.py build/debug/redis_simple
```

With `redis-server` 7 or later installed, check that each server loads the
other's AOF directory:

```sh
scripts/check_aof_interop.sh build/debug/redis_simple
```

It runs every write command this server supports, with a rewrite in
between, loads the directory in the other server, and compares the
keyspaces. Set `INTEROP_KEEP=1` to keep the directories for inspection.

With `redis-server` 7.2 or later, check that each server loads the other's
RDB file:

```sh
scripts/check_rdb_interop.py build/debug/redis_simple
```

It fills one server with every type in each of its encodings, with edge
cases such as entries at listpack size limits, saves, loads the file in the
other server, and compares every key, in both directions and without Redis'
compression and checksum.

## Formatting and clang-tidy

Format project C++ files with:

```sh
cmake --build --preset debug --target format
```

Formatting is pinned to clang-format 22 so local and CI output remains stable.
Install it with `brew install llvm` on macOS or `apt install clang-format-22`
on Ubuntu. `scripts/format.sh` discovers those installations and honors
`CLANG_FORMAT_BIN` when it points to a clang-format 22 executable.

Check formatting without modifying files with:

```sh
scripts/format.sh --check
```

Run clang-tidy with the project wrapper:

```sh
scripts/run_clang_tidy.sh
```

The wrapper checks every project file, or only the files it is given
(`scripts/run_clang_tidy.sh server/db/db.cpp`, as regular expressions). It
uses `build/debug/compile_commands.json` by default and discovers Homebrew's
LLVM toolchain on macOS. Set `BUILD_DIR`, `CLANG_TIDY_BIN`, or
`RUN_CLANG_TIDY_BIN` to override those defaults.

## Linux check from macOS

Run the Linux build and test flow locally from macOS through Docker:

```sh
scripts/run_linux_docker_check.sh
```

On its first run, the Docker helper creates a local Ubuntu 24.04 toolchain image
without mounting the repository. Build and test runs then disable container
networking, mount the repository read-only, drop Linux capabilities, and build
under a temporary filesystem. This validates the Linux `epoll` poller without
writing Linux artifacts into the working tree or exposing source code to the
container network. Set `DOCKER_BASE_IMAGE`, `DOCKER_TOOLCHAIN_IMAGE`, or
`LINUX_DOCKER_BUILD_DIR` to override the defaults.
