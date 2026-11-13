#!/usr/bin/env bash
# Measures a redis_simple server's resident memory per key after loading
# random string keys, for several value sizes, and compares two builds when
# a baseline is given.
#
# The default sizes cover std::string's inline buffer (3 bytes), the
# embedded-string range (36), and values with their own allocation (100).
#
# Usage: scripts/measure_server_memory.sh BINARY [BASELINE]
#
# Requires redis-benchmark and redis-cli. Environment overrides:
#   MEMORY_VALUE_SIZES  space-separated value sizes (default "3 36 100")
#   MEMORY_REQUESTS     SET requests per run (default 2000000)
#   MEMORY_KEY_RANGE    random key range (default 1000000)
#   MEMORY_PORT         port to use (default 8500)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 BINARY [BASELINE]" >&2
  exit 1
fi
for tool in redis-benchmark redis-cli; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "$0: ${tool} is required" >&2
    exit 1
  fi
done

binaries=("$1")
labels=("binary")
if [[ $# -eq 2 ]]; then
  binaries=("$2" "$1")
  labels=("baseline" "binary")
fi
for binary in "${binaries[@]}"; do
  if [[ ! -x "${binary}" ]]; then
    echo "$0: ${binary} is not an executable" >&2
    exit 1
  fi
done

read -r -a sizes <<<"${MEMORY_VALUE_SIZES:-3 36 100}"
requests="${MEMORY_REQUESTS:-2000000}"
key_range="${MEMORY_KEY_RANGE:-1000000}"
port="${MEMORY_PORT:-8500}"
server_pid=""

cleanup() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill -TERM "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Resident memory in KB; ps works on both Linux and macOS.
rss_kb() {
  ps -o rss= -p "${server_pid}" | tr -d ' '
}

# Sets `row` to idle and loaded RSS, key count, and bytes per key for one
# run. It runs in this shell so the exit trap can stop a failed server.
measure() {
  local binary="$1" value_size="$2"
  # Builds with RDB save points turn them off, so no background save forks
  # and no run loads an earlier one's file.
  local -a no_save=()
  if "${binary}" --help 2>&1 | grep -q -- "--save"; then
    no_save=(--save "")
  fi
  "${binary}" --port "${port}" "${no_save[@]}" >/dev/null 2>&1 &
  server_pid=$!
  for _ in $(seq 1 100); do
    if redis-cli -p "${port}" PING >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done
  local idle loaded keys
  idle="$(rss_kb)"
  # redis-benchmark warns that it cannot read CONFIG, which this server does
  # not implement; other messages still reach stderr.
  redis-benchmark -p "${port}" -q -n "${requests}" -r "${key_range}" -P 64 \
    -c 4 -d "${value_size}" -t set 2>&1 >/dev/null |
    { grep -v "Could not fetch server CONFIG" || true; } >&2
  loaded="$(rss_kb)"
  keys="$(redis-cli -p "${port}" DBSIZE)"
  kill -TERM "${server_pid}"
  wait "${server_pid}" 2>/dev/null || true
  server_pid=""
  row="$(awk -v idle="${idle}" -v loaded="${loaded}" -v keys="${keys}" \
    'BEGIN {
      printf "%10d %12d %10d %10.1f", idle, loaded, keys,
        (loaded - idle) * 1024 / keys
    }')"
}

printf '%-9s %6s %10s %12s %10s %10s\n' \
  "build" "value" "idle_kb" "loaded_kb" "keys" "bytes/key"
row=""
for size in "${sizes[@]}"; do
  for index in "${!binaries[@]}"; do
    measure "${binaries[${index}]}" "${size}"
    printf '%-9s %6s %s\n' "${labels[${index}]}" "${size}" "${row}"
  done
done
