#!/usr/bin/env bash
# Checks that redis_simple and redis-server each load the other's AOF
# directory, in the command format (no RDB preamble).
#
# Each direction writes the same data to one server, with a rewrite in the
# middle so the directory holds a base file and incremental files, records
# the keyspace, stops the server, starts the other server on the same
# directory, and compares its keyspace with the recorded one.
#
# Usage: scripts/check_aof_interop.sh REDIS_SIMPLE_BINARY [REDIS_SERVER]
#
# Requires redis-cli and a redis-server of version 7 or later (multi-part
# AOF). Set INTEROP_PORT to change the first of the two ports (default 8600),
# and INTEROP_KEEP=1 to keep the AOF directories and dumps for inspection.

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 REDIS_SIMPLE_BINARY [REDIS_SERVER]" >&2
  exit 1
fi
simple_binary="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
redis_binary="${2:-redis-server}"
for tool in redis-cli "${redis_binary}"; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "$0: ${tool} is required" >&2
    exit 1
  fi
done
if [[ ! -x "${simple_binary}" ]]; then
  echo "$0: ${simple_binary} is not an executable" >&2
  exit 1
fi

simple_port="${INTEROP_PORT:-8600}"
redis_port=$((simple_port + 1))
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/redis_simple_aof_interop.XXXXXX")"
server_pid=""

cleanup() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill -TERM "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
  if [[ -z "${INTEROP_KEEP:-}" ]]; then
    rm -rf -- "${work_dir:?}"
  else
    echo "kept ${work_dir}" >&2
  fi
}
trap cleanup EXIT INT TERM

cli() {
  local port="$1"
  shift
  redis-cli -p "${port}" "$@"
}

wait_until_ready() {
  local port="$1"
  for _ in {1..100}; do
    if [[ "$(cli "${port}" PING 2>/dev/null)" == "PONG" ]]; then
      return 0
    fi
    if ! kill -0 "${server_pid}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  echo "$0: the server on port ${port} did not start; its log:" >&2
  cat "${work_dir}/server.log" >&2
  exit 1
}

wait_for_rewrite() {
  local port="$1"
  for _ in {1..100}; do
    if cli "${port}" INFO persistence |
        grep -q '^aof_rewrite_in_progress:0'; then
      if ! cli "${port}" INFO persistence |
          grep -q '^aof_rewrite_scheduled:1'; then
        return 0
      fi
    fi
    sleep 0.1
  done
  echo "$0: the rewrite on port ${port} did not finish" >&2
  exit 1
}

# Starts a server on `port` with its AOF in directory `dir`.
start_simple() {
  local dir="$1"
  (cd "${dir}" && exec "${simple_binary}" --port "${simple_port}" \
      --appendonly yes --appenddirname appendonlydir) \
      >"${work_dir}/server.log" 2>&1 &
  server_pid=$!
  wait_until_ready "${simple_port}"
}

start_redis() {
  local dir="$1"
  "${redis_binary}" --port "${redis_port}" --dir "${dir}" --save '' \
      --appendonly yes --appenddirname appendonlydir \
      --aof-use-rdb-preamble no >"${work_dir}/server.log" 2>&1 &
  server_pid=$!
  wait_until_ready "${redis_port}"
}

stop_server() {
  kill -TERM "${server_pid}"
  wait "${server_pid}" 2>/dev/null || true
  server_pid=""
}

# Writes keys of every type, a TTL, and a transaction, with a rewrite after
# the first half so both a base file and an incremental file hold data.
write_data() {
  local port="$1"
  local large_value
  large_value="$(head -c 70000 /dev/zero | tr '\0' 'x')"
  cli "${port}" SET string hello >/dev/null
  cli "${port}" SET counter 41 >/dev/null
  cli "${port}" INCR counter >/dev/null
  cli "${port}" SET large "${large_value}" >/dev/null
  cli "${port}" RPUSH list a b c >/dev/null
  cli "${port}" LPUSH list z >/dev/null
  cli "${port}" SADD set one two three 4 >/dev/null
  cli "${port}" ZADD zset 1 a 2.5 b -3 c >/dev/null
  cli "${port}" HSET hash field value other 2 >/dev/null
  cli "${port}" SET with_ttl v EX 100000 >/dev/null
  cli "${port}" BGREWRITEAOF >/dev/null
  wait_for_rewrite "${port}"
  cli "${port}" APPEND string " world" >/dev/null
  cli "${port}" RPOP list >/dev/null
  cli "${port}" SREM set two >/dev/null
  cli "${port}" ZADD zset INCR 0.25 a >/dev/null
  cli "${port}" HDEL hash other >/dev/null
  cli "${port}" PEXPIRE list 200000000 >/dev/null
  cli "${port}" DEL counter >/dev/null
  printf 'MULTI\nSET in_transaction 1\nINCR in_transaction\nEXEC\n' |
    cli "${port}" >/dev/null
}

# Prints every key with its type, whether it has a TTL, and its value, in a
# stable order.
dump_keyspace() {
  local port="$1"
  local key type ttl
  # SCAN, since this server does not implement KEYS.
  cli "${port}" --scan | LC_ALL=C sort | while IFS= read -r key; do
    type="$(cli "${port}" --raw TYPE "${key}")"
    ttl="$(cli "${port}" --raw PTTL "${key}")"
    [[ "${ttl}" -gt 0 ]] && ttl="set"
    echo "${key} ${type} ttl=${ttl}"
    case "${type}" in
      string) cli "${port}" --raw GET "${key}" | cksum ;;
      list) cli "${port}" --raw LRANGE "${key}" 0 -1 ;;
      set) cli "${port}" --raw SMEMBERS "${key}" | LC_ALL=C sort ;;
      zset) cli "${port}" --raw ZRANGE "${key}" 0 -1 WITHSCORES ;;
      hash) cli "${port}" --raw HGETALL "${key}" | paste - - | LC_ALL=C sort ;;
    esac
  done
}

# Writes with `writer`, loads with `loader`, and compares the keyspaces.
check_direction() {
  local writer="$1" loader="$2"
  local dir="${work_dir}/${writer}_to_${loader}"
  mkdir -p "${dir}"
  "start_${writer}" "${dir}"
  local writer_port="${simple_port}" loader_port="${redis_port}"
  if [[ "${writer}" == redis ]]; then
    writer_port="${redis_port}"
    loader_port="${simple_port}"
  fi
  write_data "${writer_port}"
  dump_keyspace "${writer_port}" >"${dir}/written.txt"
  stop_server
  "start_${loader}" "${dir}"
  dump_keyspace "${loader_port}" >"${dir}/loaded.txt"
  stop_server
  if ! diff -u "${dir}/written.txt" "${dir}/loaded.txt"; then
    echo "$0: ${loader} did not load ${writer}'s AOF as written" >&2
    exit 1
  fi
  echo "${loader} loads ${writer}'s AOF: $(wc -l <"${dir}/written.txt") lines match"
}

check_direction simple redis
check_direction redis simple
echo "AOF interoperability checks passed"
