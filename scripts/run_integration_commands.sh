#!/bin/sh
set -u

server="$1"
shift

# REDIS_SIMPLE_SERVER_ARGS adds server flags, such as "--io-threads 4"; it is
# split on spaces on purpose.
# shellcheck disable=SC2086
"$server" ${REDIS_SIMPLE_SERVER_ARGS:-} &
server_pid=$!

cleanup() {
  if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

sleep 1

if ! kill -0 "$server_pid" 2>/dev/null; then
  wait "$server_pid"
  exit $?
fi

status=0
for client in "$@"; do
  "$client"
  client_status=$?
  if [ "$client_status" -ne 0 ]; then
    status="$client_status"
    break
  fi
done
if [ "$status" -ne 0 ]; then
  exit "$status"
fi

# Stop the server gracefully and fail if it exits with an error, such as a
# sanitizer report (ThreadSanitizer and LeakSanitizer report at exit).
kill -TERM "$server_pid"
wait "$server_pid"
server_status=$?
server_pid=""
exit "$server_status"
