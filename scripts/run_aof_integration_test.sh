#!/bin/sh

set -u

server="$1"
client="$2"
aof_dir="$(mktemp -d "${TMPDIR:-/tmp}/redis_simple_aof.XXXXXX")"
# A multi-part AOF directory, as in Redis 7.
aof_dirname="${aof_dir}/appendonlydir"
aof_name="appendonly.aof"
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
  "$server" --bind 127.0.0.1 --port 18081 --dir "$aof_dir" --appendonly yes \
    --appenddirname "$aof_dirname" --appendfilename "$aof_name" \
    --appendfsync everysec \
    ${REDIS_SIMPLE_SERVER_ARGS:-} "$@" &
  server_pid=$!
  sleep 1
  if ! kill -0 "$server_pid" 2>/dev/null; then
    wait "$server_pid"
    return $?
  fi
}

# Bytes in the AOF's base and incremental files.
aof_size() {
  cat "$aof_dirname"/*.aof | wc -c
}

stop_server() {
  kill -TERM "$server_pid"
  wait "$server_pid"
  server_pid=""
}

start_server || exit $?
"$client" write || exit $?
stop_server || exit $?
original_size="$(aof_size)"

sleep 1

start_server || exit $?
"$client" rewrite || exit $?
stop_server || exit $?
rewritten_size="$(aof_size)"
if [ "$rewritten_size" -ge "$original_size" ]; then
  echo "AOF rewrite did not compact command history" >&2
  exit 1
fi
manifest="${aof_dirname}/${aof_name}.manifest"
if [ "$(grep -c ' type b$' "$manifest")" -ne 1 ] ||
  grep -q ' type h$' "$manifest"; then
  echo "AOF manifest does not list one base file after the rewrite:" >&2
  cat "$manifest" >&2
  exit 1
fi

start_server || exit $?
"$client" read || exit $?
stop_server || exit $?

aof_dirname="${aof_dir}/automatic"
start_server --auto-aof-rewrite-min-size 1 \
  --auto-aof-rewrite-percentage 1 || exit $?
"$client" auto-write || exit $?
stop_server || exit $?

start_server --auto-aof-rewrite-min-size 1 \
  --auto-aof-rewrite-percentage 1 || exit $?
"$client" auto-read || exit $?
stop_server
