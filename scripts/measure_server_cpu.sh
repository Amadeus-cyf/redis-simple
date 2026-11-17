#!/usr/bin/env bash
# Server CPU time for one redis-benchmark workload of your choice, for a
# binary and a baseline. It stands in for profile_server.sh where callgrind
# does not run (macOS), and covers workloads that script does not have. CPU
# time varies far less than throughput on a busy machine, but it is still a
# timing, so runs alternate between the two binaries and the minimum, median,
# and maximum of each are printed.
#
# Usage: measure_server_cpu.sh PORT RUNS BINARY BASELINE -- \
#          redis-benchmark arguments
# Example:
#   scripts/measure_server_cpu.sh 7460 7 build/release/redis_simple \
#     path/to/baseline/redis_simple -- -t set,get -n 2000000 -P 16
#
# Each run starts a fresh server in an empty directory with saving off, runs
# the optional preload, then measures the user plus system CPU seconds the
# server process spends on the workload.
#
# Environment:
#   SERVER_ARGS  extra server arguments, for example "--appendonly yes"
#   PRELOAD      redis-benchmark arguments to run before measuring
#   PRELOAD_CLI  a redis-cli command line to run before measuring
#
# Needs redis-benchmark and redis-cli on PATH.

set -euo pipefail

if [[ $# -lt 6 || "$5" != "--" ]]; then
  echo "usage: $0 PORT RUNS BINARY BASELINE -- redis-benchmark arguments" >&2
  exit 1
fi
port="$1"
runs="$2"
binary="$3"
baseline="$4"
shift 5
workload=("$@")
for server in "${binary}" "${baseline}"; do
  if [[ ! -x "${server}" ]]; then
    echo "not an executable server binary: ${server}" >&2
    exit 1
  fi
done

read -r -a server_args <<<"${SERVER_ARGS:-}"
read -r -a preload <<<"${PRELOAD:-}"
read -r -a preload_cli <<<"${PRELOAD_CLI:-}"

server_pid=""
data_dir=""
stop_server() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill -TERM "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
  server_pid=""
  if [[ -n "${data_dir}" ]]; then
    rm -rf "${data_dir}"
    data_dir=""
  fi
}
trap stop_server EXIT INT TERM

# User plus system CPU seconds of the server so far.
cpu_seconds() {
  if [[ -r "/proc/${server_pid}/stat" ]]; then
    # Linux: fields 14 and 15 are clock ticks. The command name in field 2
    # may hold spaces, so count from its closing parenthesis.
    awk -v ticks="$(getconf CLK_TCK)" '{
      sub(/^.*\) /, "")
      printf "%.2f", ($12 + $13) / ticks
    }' "/proc/${server_pid}/stat"
    return
  fi
  # macOS: ps prints each time as [HH:]MM:SS.cc.
  ps -o utime=,stime= -p "${server_pid}" | awk '{
    total = 0
    for (i = 1; i <= NF; i++) {
      n = split($i, part, ":")
      seconds = 0
      for (j = 1; j <= n; j++) seconds = seconds * 60 + part[j]
      total += seconds
    }
    printf "%.2f", total
  }'
}

measure() {
  local server="$1"
  data_dir="$(mktemp -d)"
  "${server}" --port "${port}" --dir "${data_dir}" --save "" \
    ${server_args[@]+"${server_args[@]}"} >/dev/null 2>&1 &
  server_pid=$!
  local attempt started=""
  for attempt in $(seq 1 100); do
    if redis-cli -p "${port}" PING 2>/dev/null | grep -q PONG; then
      started="yes"
      break
    fi
    sleep 0.05
  done
  if [[ -z "${started}" ]]; then
    echo "${server} did not start on port ${port}" >&2
    exit 1
  fi
  if [[ ${#preload[@]} -gt 0 ]]; then
    redis-benchmark -p "${port}" -q "${preload[@]}" >/dev/null 2>&1
  fi
  if [[ ${#preload_cli[@]} -gt 0 ]]; then
    redis-cli -p "${port}" "${preload_cli[@]}" >/dev/null
  fi
  local before after
  before="$(cpu_seconds)"
  # redis-benchmark warns on stderr that the server has no CONFIG command.
  redis-benchmark -p "${port}" -q "${workload[@]}" >/dev/null 2>&1
  after="$(cpu_seconds)"
  stop_server
  awk -v a="${after}" -v b="${before}" 'BEGIN { printf "%.2f", a - b }'
}

summary() {
  printf '%s\n' "$@" | sort -n | awk '{
    value[NR] = $1
  } END {
    middle = (NR % 2) ? value[(NR + 1) / 2] : (value[NR / 2] + value[NR / 2 + 1]) / 2
    printf "min %.2f  median %.2f  max %.2f", value[1], middle, value[NR]
  }'
}

baseline_runs=()
binary_runs=()
for run in $(seq 1 "${runs}"); do
  baseline_runs+=("$(measure "${baseline}")")
  binary_runs+=("$(measure "${binary}")")
done

echo "workload: ${workload[*]}"
echo "baseline: ${baseline_runs[*]}  ($(summary "${baseline_runs[@]}"))"
echo "binary:   ${binary_runs[*]}  ($(summary "${binary_runs[@]}"))"
awk -v a="$(printf '%s\n' "${baseline_runs[@]}" | sort -n | head -1)" \
  -v b="$(printf '%s\n' "${binary_runs[@]}" | sort -n | head -1)" \
  'BEGIN { printf "change of the minimum: %+.1f%%\n", (b - a) / a * 100 }'
