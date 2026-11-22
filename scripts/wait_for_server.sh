# Sourced by the integration runners: wait_for_server PID PORT waits until
# the server with process ID PID accepts connections on 127.0.0.1:PORT.
#
# It polls with nc every 50 ms, for 30 seconds or a little more, so a fast
# build starts its clients at once and a slow one (a sanitizer build on a busy
# machine) is not failed by a fixed wait. A port that accepts is not proof
# that this server holds it: another process may, and a server that cannot
# bind exits a moment later. So the PID is checked once more 200 ms after the
# port accepts. Where nc has no -z, or sleep takes whole seconds only, it
# waits one second instead, as the runners did before.
#
# Returns nonzero with the server stopped and reaped, so the caller has
# nothing left to stop: the server's exit status (1 if that was 0) if it
# exited, or 1, having killed it, if it never accepted connections.
wait_for_server() {
  wait_pid="$1"
  wait_port="$2"
  if wait_for_server_can_poll; then
    wait_attempt=0
    while kill -0 "$wait_pid" 2>/dev/null; do
      if nc -z 127.0.0.1 "$wait_port" >/dev/null 2>&1; then
        sleep 0.2
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

# Whether this system's nc can probe a port (-z) and its sleep takes
# fractions of a second; POSIX requires neither.
wait_for_server_can_poll() {
  command -v nc >/dev/null 2>&1 || return 1
  nc -h 2>&1 | grep -q -e '-z' || return 1
  sleep 0.05 2>/dev/null
}
