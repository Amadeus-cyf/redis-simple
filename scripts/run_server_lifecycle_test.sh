#!/bin/sh

set -u

server="$1"
port="18080"

if ! "$server" --help >/dev/null; then
  echo "server --help failed" >&2
  exit 1
fi
if "$server" --port 0 >/dev/null 2>&1; then
  echo "server accepted an invalid port" >&2
  exit 1
fi
# As in Redis, dbfilename is a name inside dir, and dir must exist.
if "$server" --dbfilename sub/dump.rdb >/dev/null 2>&1; then
  echo "server accepted a dbfilename with a directory" >&2
  exit 1
fi
if "$server" --port "$port" --dir /nonexistent/redis_simple >/dev/null 2>&1; then
  echo "server started in a missing dir" >&2
  exit 1
fi
if "$server" --rdbcompression maybe >/dev/null 2>&1; then
  echo "server accepted an invalid rdbcompression" >&2
  exit 1
fi

"$server" --bind 127.0.0.1 --port "$port" &
server_pid=$!

cleanup() {
  if kill -0 "$server_pid" 2>/dev/null; then
    kill -KILL "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

sleep 1
if ! kill -0 "$server_pid" 2>/dev/null; then
  wait "$server_pid"
  exit $?
fi

kill -TERM "$server_pid"
attempt=0
while kill -0 "$server_pid" 2>/dev/null && [ "$attempt" -lt 30 ]; do
  sleep 0.1
  attempt=$((attempt + 1))
done
if kill -0 "$server_pid" 2>/dev/null; then
  echo "server did not stop after SIGTERM" >&2
  exit 1
fi

wait "$server_pid"
status=$?
trap - EXIT INT TERM
exit "$status"
