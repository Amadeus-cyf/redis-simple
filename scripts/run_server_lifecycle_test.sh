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
# Redis' save takes pairs of seconds (at least 1) and changes.
for save in "60" "0 1" "60 -1" "60 x"; do
  if "$server" --save "$save" >/dev/null 2>&1; then
    echo "server accepted save parameters \"$save\"" >&2
    exit 1
  fi
done
if "$server" --stop-writes-on-bgsave-error maybe >/dev/null 2>&1; then
  echo "server accepted an invalid stop-writes-on-bgsave-error" >&2
  exit 1
fi

# The server saves an RDB file at shutdown, so it runs in a directory of its
# own.
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/redis_simple_lifecycle.XXXXXX")"
"$server" --bind 127.0.0.1 --port "$port" --dir "$work_dir" &
server_pid=$!

cleanup() {
  if kill -0 "$server_pid" 2>/dev/null; then
    kill -KILL "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$work_dir"
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
rm -rf "$work_dir"
exit "$status"
