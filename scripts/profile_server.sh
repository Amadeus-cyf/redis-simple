#!/usr/bin/env bash
# Counts the instructions a redis_simple server executes for fixed workloads,
# using callgrind, and compares two builds when a baseline is given.
#
# Instruction counts barely vary between runs, so they show differences of a
# few percent that wall-clock benchmarks on a busy or small machine cannot.
# They do not capture cache-miss costs, so confirm large changes with
# redis-benchmark throughput on a quiet machine.
#
# Usage: scripts/profile_server.sh [-w workload[,workload...]] BINARY [BASELINE]
#
# Requires valgrind (Linux), redis-benchmark, and redis-cli. Set PROFILE_PORT
# to change the first port used (default 8200) and PROFILE_OUTPUT_DIR to keep
# the callgrind files for callgrind_annotate (default: a temporary directory).

set -euo pipefail

readonly ALL_WORKLOADS="small-mix,pipelined-get,unpipelined,large-values,mid-values,collections,aof-writes,key-expiry,rdb-save-load,aof-load"

usage() {
  echo "usage: $0 [-w workload[,workload...]] BINARY [BASELINE]" >&2
  echo "workloads: ${ALL_WORKLOADS}" >&2
  exit 1
}

workloads="${ALL_WORKLOADS}"
while getopts "w:h" option; do
  case "${option}" in
    w) workloads="${OPTARG}" ;;
    *) usage ;;
  esac
done
shift $((OPTIND - 1))
if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
fi

for tool in valgrind callgrind_annotate redis-benchmark redis-cli; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "$0: ${tool} is required" >&2
    exit 1
  fi
done

binaries=("$1")
if [[ $# -eq 2 ]]; then
  binaries=("$2" "$1")
fi
for binary in "${binaries[@]}"; do
  if [[ ! -x "${binary}" ]]; then
    echo "$0: ${binary} is not an executable" >&2
    exit 1
  fi
done

output_dir="${PROFILE_OUTPUT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/redis_simple_profile.XXXXXX")}"
mkdir -p "${output_dir}"
port="${PROFILE_PORT:-8200}"
server_pid=""

cleanup() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill -TERM "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

cli() {
  redis-cli -p "${port}" "$@"
}

# redis-benchmark warns that it cannot read CONFIG, which this server does
# not implement; other messages still reach stderr.
bench() {
  redis-benchmark -p "${port}" -q "$@" 2>&1 >/dev/null |
    { grep -v "Could not fetch server CONFIG" || true; } >&2
}

# Starts BINARY under callgrind, writing to OUTPUT, with extra server flags.
# Builds with RDB save points turn them off, so no background save or save at
# shutdown adds to the counts, and no run loads an earlier one's file.
start_server() {
  local binary="$1" output="$2"
  shift 2
  local -a no_save=()
  if "${binary}" --help 2>&1 | grep -q -- "--save"; then
    no_save=(--save "")
  fi
  valgrind --tool=callgrind --callgrind-out-file="${output}" \
    "${binary}" --port "${port}" "${no_save[@]}" "$@" >/dev/null 2>&1 &
  server_pid=$!
  for _ in $(seq 1 150); do
    if cli PING >/dev/null 2>&1; then
      return 0
    fi
    if ! kill -0 "${server_pid}" 2>/dev/null; then
      break
    fi
    sleep 0.2
  done
  echo "$0: server did not start: ${binary}" >&2
  exit 1
}

# Stops the server gracefully so callgrind writes its counts.
stop_server() {
  kill -TERM "${server_pid}"
  wait "${server_pid}" 2>/dev/null || true
  server_pid=""
  port=$((port + 1))
}

# Fills a hash, set, and sorted set past their listpack limits, then reads
# them whole and by range.
load_collections() {
  local i
  for i in $(seq 1 1000); do
    echo "HSET profile:hash field:${i} value:${i}"
    echo "SADD profile:set member:${i}"
    echo "ZADD profile:zset ${i}.5 member:${i}"
  done | cli >/dev/null
  for i in $(seq 1 200); do
    echo "HGETALL profile:hash"
    echo "SMEMBERS profile:set"
    echo "ZRANGE profile:zset 0 -1 WITHSCORES"
    echo "ZRANGEBYSCORE profile:zset 100 600"
    echo "HGET profile:hash field:${i}"
    echo "ZSCORE profile:zset member:${i}"
  done | cli >/dev/null
}

# Sets keys with TTLs, updates and reads them, then scans with a pattern.
load_key_expiry() {
  local i
  for i in $(seq 1 3000); do
    echo "SET profile:key:${i} value PX 600000"
    echo "EXPIRE profile:key:${i} 900"
    echo "TTL profile:key:${i}"
    echo "EXISTS profile:key:${i} profile:key:$((i + 1))"
    echo "DEL profile:missing:${i}"
  done | cli >/dev/null
  for _ in $(seq 1 100); do
    echo "SCAN 0 COUNT 500 MATCH profile:key:1*"
  done | cli >/dev/null
}

# Runs WORKLOAD against BINARY and sets `instructions` to its total. It runs
# in this shell, not a subshell, so the exit trap can stop a failed server.
profile() {
  local binary="$1" workload="$2" tag="$3"
  local output="${output_dir}/${tag}-${workload}.cg"
  # Callgrind files of further servers the workload starts, counted too.
  local -a extra_outputs=()
  case "${workload}" in
    small-mix)
      start_server "${binary}" "${output}"
      bench -n 30000 -c 10 -P 16 -r 10000 \
        -t set,get,incr,lpush,rpop,sadd,hset,zadd,mset
      ;;
    pipelined-get)
      start_server "${binary}" "${output}"
      bench -n 60000 -c 10 -P 50 -r 10000 -t get
      ;;
    unpipelined)
      start_server "${binary}" "${output}"
      bench -n 6000 -c 10 -r 10000 -t set,get
      ;;
    large-values)
      start_server "${binary}" "${output}"
      bench -n 6000 -c 10 -P 8 -d 4096 -r 1000 -t set,get
      ;;
    mid-values)
      start_server "${binary}" "${output}"
      bench -n 60000 -c 10 -P 16 -d 36 -r 100000 -t set,get
      ;;
    collections)
      start_server "${binary}" "${output}"
      load_collections
      ;;
    aof-writes)
      # Builds with the multi-part AOF take a directory; older ones a path.
      local aof="${output_dir}/${tag}-aof.aof"
      local -a aof_args=(--appendfilename "${aof}")
      # The usage text comes with a failing exit status, which pipefail
      # would carry into the test, so capture it first.
      local usage
      usage="$("${binary}" --no-such-option 2>&1 || true)"
      if [[ "${usage}" == *--appenddirname* ]]; then
        aof_args=(--appenddirname "${output_dir}/${tag}-aof"
          --appendfilename appendonly.aof)
      fi
      start_server "${binary}" "${output}" --appendonly yes \
        "${aof_args[@]}" --appendfsync no
      bench -n 30000 -c 10 -P 16 -r 10000 -t set,incr,lpush,hset,zadd
      ;;
    key-expiry)
      start_server "${binary}" "${output}"
      load_key_expiry
      ;;
    aof-load)
      # Writes with the AOF on, as aof-writes does, then replays the file in
      # new servers. Writing costs the same as in aof-writes, so a difference
      # beyond that workload's comes from loading.
      local aof_dir="${output_dir}/${tag}-aof"
      local -a aof_load_args=(--appendonly yes --appenddirname "${aof_dir}"
        --appendfilename appendonly.aof --appendfsync no)
      start_server "${binary}" "${output}" "${aof_load_args[@]}"
      bench -n 30000 -c 10 -P 16 -r 10000 -t set,incr,lpush,hset,zadd
      local replay
      for replay in 1 2 3; do
        stop_server
        extra_outputs+=("${output}.load${replay}")
        start_server "${binary}" "${output}.load${replay}" \
          "${aof_load_args[@]}"
      done
      ;;
    rdb-save-load)
      # Fills the database with every type, saves it repeatedly, then loads
      # the file in new servers. Filling costs the same in both builds, so
      # a difference comes from saving or loading.
      local dir="${output_dir}/${tag}-rdb"
      mkdir -p "${dir}"
      start_server "${binary}" "${output}" --dir "${dir}"
      bench -n 20000 -c 10 -P 16 -r 20000 -t set,lpush,sadd,hset,zadd
      for _ in $(seq 1 20); do
        cli SAVE >/dev/null
      done
      local load
      for load in 1 2 3; do
        stop_server
        extra_outputs+=("${output}.load${load}")
        start_server "${binary}" "${output}.load${load}" --dir "${dir}"
      done
      ;;
    *)
      echo "$0: unknown workload ${workload}" >&2
      exit 1
      ;;
  esac
  stop_server
  rm -rf "${output_dir}/${tag}-aof.aof"* "${output_dir}/${tag}-aof" \
    "${output_dir}/${tag}-rdb"
  instructions=0
  local file count
  for file in "${output}" ${extra_outputs[@]+"${extra_outputs[@]}"}; do
    count="$(callgrind_annotate "${file}" 2>/dev/null |
      awk '/PROGRAM TOTALS/ { gsub(",", "", $1); print $1 }')"
    instructions=$((instructions + count))
  done
}

IFS=',' read -r -a selected <<<"${workloads}"
if [[ ${#binaries[@]} -eq 1 ]]; then
  printf '%-14s %15s\n' "workload" "instructions"
else
  printf '%-14s %15s %15s %8s\n' "workload" "baseline" "binary" "change"
fi
instructions=""
for workload in "${selected[@]}"; do
  if [[ ${#binaries[@]} -eq 1 ]]; then
    profile "${binaries[0]}" "${workload}" binary
    printf '%-14s %15s\n' "${workload}" "${instructions}"
  else
    profile "${binaries[0]}" "${workload}" baseline
    baseline_count="${instructions}"
    profile "${binaries[1]}" "${workload}" binary
    awk -v name="${workload}" -v base="${baseline_count}" \
      -v new="${instructions}" 'BEGIN {
        printf "%-14s %15d %15d %+7.1f%%\n", name, base, new,
          (new - base) * 100 / base
      }'
  fi
done
echo "callgrind files: ${output_dir}" >&2
