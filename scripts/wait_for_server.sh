# Sourced by the integration runners: wait_for_server PID PORT waits until
# the server with process ID PID accepts connections on 127.0.0.1:PORT.
#
# It polls with nc every 50 ms, for up to 30 seconds, so a fast build starts
# its clients at once and a slow one (a sanitizer build on a busy machine)
# is not failed by a fixed wait. Without nc it waits one second, as the
# runners did before. Returns nonzero, having reaped the server, if the
# server exits first: its exit status, or 1 if that was 0.
wait_for_server() {
  wait_pid="$1"
  wait_port="$2"
  if ! command -v nc >/dev/null 2>&1; then
    sleep 1
    if kill -0 "$wait_pid" 2>/dev/null; then
      return 0
    fi
  fi
  wait_attempt=0
  while true; do
    if ! kill -0 "$wait_pid" 2>/dev/null; then
      wait "$wait_pid"
      wait_status=$?
      # A server that exited successfully before serving is still a failure.
      [ "$wait_status" -ne 0 ] || wait_status=1
      return "$wait_status"
    fi
    if nc -z 127.0.0.1 "$wait_port" >/dev/null 2>&1; then
      return 0
    fi
    wait_attempt=$((wait_attempt + 1))
    if [ "$wait_attempt" -ge 600 ]; then
      echo "the server did not accept connections on port ${wait_port}" >&2
      return 1
    fi
    sleep 0.05
  done
}
