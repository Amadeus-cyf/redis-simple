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
  if kill -0 "$server_pid" 2>/dev/null; then
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

exit "$status"
