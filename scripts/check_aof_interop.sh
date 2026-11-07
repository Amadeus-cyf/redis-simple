#!/usr/bin/env bash
# Checks that redis_simple and redis-server each load the other's AOF
# directory, in the command format (no RDB preamble).
#
# Each direction runs every supported write command on one server, with a
# rewrite in the middle so the directory holds a base file and incremental
# files, records the keyspace, stops the server, starts the other server on
# the same directory, and compares its keyspace with the recorded one.
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

# Runs every write command this server supports, with its options, in both
# the base file (before the rewrite) and the incremental file (after it).
# Redis records some commands in another form than they were sent (SET EX as
# SET PXAT, EXPIRE as PEXPIREAT, a pop on the last element as a pop plus
# DEL), so this checks that each server loads the other's forms.
write_data() {
  local port="$1"
  local large_value
  large_value="$(head -c 70000 /dev/zero | tr '\0' 'x')"
  cli "${port}" SET large "${large_value}" >/dev/null
  cli "${port}" >/dev/null <<'COMMANDS'
SET string hello
SET counter 41
INCR counter
RPUSH list a b c d e f g
LPUSH list z
SADD set one two three 4
ZADD zset 1 a 2.5 b -3 c 0.1 d
HSET hash field value other 2 count 10
SET with_ttl v EX 100000
COMMANDS
  cli "${port}" BGREWRITEAOF >/dev/null
  wait_for_rewrite "${port}"
  cli "${port}" >/dev/null <<'COMMANDS'
APPEND string " world"
APPEND new_by_append abc
INCRBY counter 5
DECR counter
DECRBY counter 3
INCR fresh_counter
SET set_ex v EX 100000
SET set_px v PX 100000000
SET set_exat v EXAT 4102444800
SET set_pxat v PXAT 4102444800000
SET with_ttl replaced KEEPTTL
SET string_nx v NX
SET string_nx ignored NX
SET string_xx v XX
SET string hello2 GET
SET plain_after_ttl v EX 100
SET plain_after_ttl v
MSET m1 1 m2 two m3 3.5
RPOP list
RPOP list 2
LPOP list
LSET list 0 first
LREM list 0 c
LTRIM list 0 1
RPUSH popped_empty x
LPOP popped_empty
RPUSH popped_count x y
RPOP popped_count 5
SREM set two
SADD set five
SADD removed_set only
SREM removed_set only
ZADD zset INCR 0.25 a
ZADD zset GT CH 3 a 1 b
ZADD zset LT 0 d
ZADD zset NX 7 e 8 a
ZADD zset XX 9 e 9.5 b
ZREM zset c
ZADD removed_zset 1 only
ZREM removed_zset only
HSET hash added yes
HMSET hash bulk1 1 bulk2 2
HINCRBY hash count -3
HINCRBY hash new_count 4
HDEL hash other
HSET removed_hash f v
HDEL removed_hash f
PEXPIRE list 200000000
EXPIRE set 100000
EXPIREAT zset 4102444800
PEXPIREAT hash 4102444800000
EXPIRE set 200000 GT
EXPIRE set 1 GT
EXPIRE zset 50000 LT
EXPIRE string 100000 NX
EXPIRE m1 100000 XX
SET persisted v EX 100000
PERSIST persisted
SET expired_now v
EXPIRE expired_now 0
SET expired_past v
PEXPIREAT expired_past 1
SET renamed_from v EX 100000
RENAME renamed_from renamed_to
SET deleted v
DEL deleted counter
SET unlinked v
UNLINK unlinked
MULTI
SET in_transaction 1
INCR in_transaction
RPUSH in_transaction_list a
EXEC
COMMANDS
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
