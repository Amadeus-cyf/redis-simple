#!/bin/sh

set -u

server="$1"
client="$2"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/redis_simple_rdb.XXXXXX")"
rdb_name="snapshot.rdb"
server_pid=""

cleanup() {
  if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM

# REDIS_SIMPLE_SERVER_ARGS adds server flags, such as "--io-threads 4"; it is
# split on spaces on purpose.
start_server() {
  # shellcheck disable=SC2086
  "$server" --bind 127.0.0.1 --port 18082 --dir "$work_dir" \
    --dbfilename "$rdb_name" ${REDIS_SIMPLE_SERVER_ARGS:-} "$@" &
  server_pid=$!
  sleep 1
  if ! kill -0 "$server_pid" 2>/dev/null; then
    wait "$server_pid"
    status=$?
    server_pid=""
    return "$status"
  fi
}

stop_server() {
  kill -TERM "$server_pid"
  wait "$server_pid"
  status=$?
  server_pid=""
  return "$status"
}

# No file yet: the server starts empty.
start_server || exit $?
"$client" empty || exit $?
"$client" write || exit $?
stop_server || exit $?

# SAVE wrote the file in --dir under --dbfilename, in Redis' format, and left
# no temporary file behind.
rdb_path="${work_dir}/${rdb_name}"
if [ "$(head -c 9 "$rdb_path")" != "REDIS0011" ]; then
  echo "SAVE did not write an RDB file at ${rdb_path}" >&2
  exit 1
fi
if ls "$work_dir"/temp-*.rdb >/dev/null 2>&1; then
  echo "SAVE left a temporary file behind" >&2
  exit 1
fi

# A restart loads it.
start_server || exit $?
"$client" read || exit $?
stop_server || exit $?

# The same data saved without compression or a checksum loads too.
start_server --rdbcompression no --rdbchecksum no || exit $?
"$client" write || exit $?
stop_server || exit $?
start_server || exit $?
"$client" read || exit $?
stop_server || exit $?

# A damaged file stops the server instead of starting it with partial data.
size="$(wc -c <"$rdb_path")"
head -c "$((size / 2))" "$rdb_path" >"${rdb_path}.cut"
mv "${rdb_path}.cut" "$rdb_path"
if start_server; then
  echo "the server started from a truncated RDB file" >&2
  exit 1
fi
exit 0
