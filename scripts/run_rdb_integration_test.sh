#!/bin/sh

set -u

server="$1"
client="$2"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/redis_simple_rdb.XXXXXX")"
rdb_name="snapshot.rdb"
server_pid=""
# Each phase runs the server in a directory of its own.
server_dir="$work_dir"

cleanup() {
  if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill -KILL "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM

# REDIS_SIMPLE_SERVER_ARGS adds server flags, such as "--io-threads 4"; it is
# split on spaces on purpose.
start_server() {
  # shellcheck disable=SC2086
  "$server" --bind 127.0.0.1 --port 18082 --dir "$server_dir" \
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

# SAVE alone, with save points off, so nothing is saved at shutdown. No
# file yet: the server starts empty.
start_server --save "" || exit $?
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
start_server --save "" || exit $?
"$client" read || exit $?
stop_server || exit $?

# The same data saved without compression or a checksum loads too.
start_server --save "" --rdbcompression no --rdbchecksum no || exit $?
"$client" write || exit $?
stop_server || exit $?
start_server --save "" || exit $?
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

# BGSAVE with the default save points, then the save at shutdown, which a
# restart loads.
server_dir="${work_dir}/background"
mkdir "$server_dir"
start_server || exit $?
"$client" bgsave || exit $?
stop_server || exit $?
start_server || exit $?
"$client" shutdown || exit $?
stop_server || exit $?

# A save point starts a background save on its own.
server_dir="${work_dir}/save_point"
mkdir "$server_dir"
start_server --save "1 1" || exit $?
"$client" savepoint || exit $?
stop_server || exit $?

# Saves fail once the directory is gone. Writes are then refused, and the
# server will not exit on SIGTERM, since its final save fails too, as in
# Redis.
server_dir="${work_dir}/misconf"
mkdir "$server_dir"
start_server --save "1 1" || exit $?
rm -rf "$server_dir"
"$client" misconf || exit $?
kill -TERM "$server_pid"
sleep 1
if ! kill -0 "$server_pid" 2>/dev/null; then
  echo "the server exited although its final save failed" >&2
  exit 1
fi
kill -KILL "$server_pid"
wait "$server_pid" 2>/dev/null
server_pid=""

# With --stop-writes-on-bgsave-error no, writes continue.
server_dir="${work_dir}/no_stop"
mkdir "$server_dir"
start_server --save "1 1" --stop-writes-on-bgsave-error no || exit $?
rm -rf "$server_dir"
"$client" nostop || exit $?
kill -KILL "$server_pid"
wait "$server_pid" 2>/dev/null
server_pid=""
exit 0
