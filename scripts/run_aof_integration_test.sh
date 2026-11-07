#!/bin/sh

set -u

server="$1"
client="$2"
aof_dir="$(mktemp -d "${TMPDIR:-/tmp}/redis_simple_aof.XXXXXX")"
aof_file="${aof_dir}/appendonly.aof"
server_pid=""

cleanup() {
  if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$aof_dir"
}
trap cleanup EXIT INT TERM

# REDIS_SIMPLE_SERVER_ARGS adds server flags, such as "--io-threads 4"; it is
# split on spaces on purpose.
start_server() {
  # shellcheck disable=SC2086
  "$server" --bind 127.0.0.1 --port 18081 --appendonly yes \
    --appendfilename "$aof_file" --appendfsync everysec \
    ${REDIS_SIMPLE_SERVER_ARGS:-} "$@" &
  server_pid=$!
  sleep 1
  if ! kill -0 "$server_pid" 2>/dev/null; then
    wait "$server_pid"
    return $?
  fi
}

stop_server() {
  kill -TERM "$server_pid"
  wait "$server_pid"
  server_pid=""
}

start_server || exit $?
"$client" write || exit $?
stop_server || exit $?
original_size="$(wc -c < "$aof_file")"

sleep 1

start_server || exit $?
"$client" rewrite || exit $?
stop_server || exit $?
rewritten_size="$(wc -c < "$aof_file")"
if [ "$rewritten_size" -ge "$original_size" ]; then
  echo "AOF rewrite did not compact command history" >&2
  exit 1
fi

start_server || exit $?
"$client" read || exit $?
stop_server || exit $?

aof_file="${aof_dir}/automatic.aof"
start_server --auto-aof-rewrite-min-size 1 \
  --auto-aof-rewrite-percentage 1 || exit $?
"$client" auto-write || exit $?
stop_server || exit $?

start_server --auto-aof-rewrite-min-size 1 \
  --auto-aof-rewrite-percentage 1 || exit $?
"$client" auto-read || exit $?
stop_server
