# Sourced by the integration runners: wait_for_server PID PORT waits until
# the server with process ID PID accepts connections on 127.0.0.1:PORT.
#
# It polls with nc every 50 ms, for 30 seconds or a little more, so a fast
# build starts its clients at once and a slow one (a sanitizer build on a busy
# machine) is not failed by a fixed wait. Where nc has no -z, or sleep takes
# whole seconds only, it waits one second instead, as the runners did before.
#
# A port that accepts does not show which process holds it, so the runners
# call require_free_port before they start a server.
#
# Returns nonzero with the server stopped and reaped, so the caller has
# nothing left to stop: the server's exit status (1 if that was 0) if it
# exited, or 1, having killed it, if it never accepted connections.
wait_for_server() {
  wait_pid="$1"
  wait_port="$2"
  if wait_for_server_can_probe && sleep 0.05 2>/dev/null; then
    wait_attempt=0
    while kill -0 "$wait_pid" 2>/dev/null; do
      if nc -z 127.0.0.1 "$wait_port" >/dev/null 2>&1; then
        break
      fi
      wait_attempt=$((wait_attempt + 1))
      if [ "$wait_attempt" -ge 600 ]; then
        echo "the server did not accept connections on port ${wait_port}" >&2
        kill -KILL "$wait_pid" 2>/dev/null
        wait "$wait_pid" 2>/dev/null
        return 1
      fi
      sleep 0.05
    done
  else
    sleep 1
  fi
  if kill -0 "$wait_pid" 2>/dev/null; then
    return 0
  fi
  wait "$wait_pid"
  wait_status=$?
  [ "$wait_status" -ne 0 ] || wait_status=1
  return "$wait_status"
}

# require_free_port PORT fails, with a message, if a process already accepts
# connections on 127.0.0.1:PORT, such as a server an earlier run left behind.
# A server started then cannot bind and exits, while the other process
# answers the tests. Call it before starting a server, and exit on failure
# rather than treat it as the server's: a test that expects the server to
# refuse to start would otherwise pass. Where nc cannot probe, it checks
# nothing.
require_free_port() {
  if wait_for_server_can_probe &&
    nc -z 127.0.0.1 "$1" >/dev/null 2>&1; then
    echo "port $1 is already in use; stop the process that holds it" >&2
    return 1
  fi
  return 0
}

# Whether this system's nc can probe a port (-z); POSIX does not require it.
wait_for_server_can_probe() {
  command -v nc >/dev/null 2>&1 || return 1
  nc -h 2>&1 | grep -q -e '-z'
}
