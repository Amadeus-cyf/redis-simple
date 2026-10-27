#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case "$(uname -s)" in
  Darwin)
    test_binary="${1:-$root_dir/build/debug/redis_simple_tests}"
    if [ ! -x "$test_binary" ]; then
      echo "unit test binary not found: $test_binary" >&2
      echo "run cmake --preset debug and cmake --build --preset debug first" >&2
      exit 1
    fi

    output_file=$(mktemp "${TMPDIR:-/tmp}/redis_simple_leaks.XXXXXX")
    trap 'rm -f "$output_file"' EXIT INT TERM
    # Death tests fork children that abort on purpose, which stalls `leaks`.
    # Skip them here; the Linux LeakSanitizer run still covers them.
    if ! MallocStackLogging=1 leaks --atExit -- "$test_binary" \
        --gtest_filter='-*DeathTest.*' >"$output_file" 2>&1; then
      cat "$output_file"
      exit 1
    fi
    cat "$output_file"
    if ! grep -Eq 'Process [0-9]+: 0 leaks for 0 total leaked bytes\.' \
        "$output_file"; then
      echo "macOS leak check did not report a clean result" >&2
      exit 1
    fi
    ;;
  Linux)
    leak_options="detect_leaks=1:halt_on_error=1"
    undefined_options="halt_on_error=1:print_stacktrace=1"
    if [ -n "${ASAN_OPTIONS:-}" ]; then
      ASAN_OPTIONS="$ASAN_OPTIONS:$leak_options"
    else
      ASAN_OPTIONS="$leak_options"
    fi
    if [ -n "${UBSAN_OPTIONS:-}" ]; then
      UBSAN_OPTIONS="$UBSAN_OPTIONS:$undefined_options"
    else
      UBSAN_OPTIONS="$undefined_options"
    fi
    export ASAN_OPTIONS UBSAN_OPTIONS

    cd "$root_dir"
    exec ctest --preset sanitizer --output-on-failure
    ;;
  *)
    echo "unsupported leak-check platform: $(uname -s)" >&2
    exit 1
    ;;
esac
