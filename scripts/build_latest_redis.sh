#!/usr/bin/env bash
#
# Builds the latest stable Redis release from source, as the reference server
# for the differential and interoperability scripts. Nothing is installed:
# the binaries stay in DIRECTORY/src.
#
# Usage: scripts/build_latest_redis.sh DIRECTORY
#        scripts/build_latest_redis.sh --print-version
#
# Prints the directory holding redis-server and redis-cli on the last line.
# Set REDIS_VERSION to build a specific release instead of the latest.

set -euo pipefail

repository="https://github.com/redis/redis.git"

latest_version() {
  # Release tags are plain MAJOR.MINOR.PATCH; release candidates and
  # milestones carry a suffix and are skipped.
  git ls-remote --tags --refs "${repository}" |
    awk -F/ '{print $3}' |
    grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' |
    sort -V |
    tail -n 1
}

if [[ $# -ne 1 ]]; then
  echo "usage: $0 DIRECTORY | --print-version" >&2
  exit 1
fi

version="${REDIS_VERSION:-$(latest_version)}"
if [[ -z "${version}" ]]; then
  echo "$0: could not find the latest Redis release" >&2
  exit 1
fi
if [[ "$1" == "--print-version" ]]; then
  echo "${version}"
  exit 0
fi

directory="$1"
if [[ ! -x "${directory}/src/redis-server" ]] ||
    [[ "$("${directory}/src/redis-server" --version)" != *"v=${version} "* ]]; then
  rm -rf "${directory}"
  git clone --quiet --depth 1 --branch "${version}" "${repository}" \
    "${directory}"
  jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
  # One make in src/ for both programs: the top-level Makefile runs a make
  # per goal, and two at once build the same dependencies in deps/ at the
  # same time, which sometimes fails.
  make -C "${directory}/src" -j"${jobs}" BUILD_TLS=no MALLOC=libc \
    redis-server redis-cli >"${directory}/build.log" 2>&1 || {
    # With parallel jobs, the error may be well before the end.
    # grep finds nothing in some failed builds, which must not end the
    # script before the tail below.
    grep -n -E "error|Error [0-9]" "${directory}/build.log" |
      head -n 20 >&2 || true
    tail -n 40 "${directory}/build.log" >&2
    exit 1
  }
fi

echo "Redis ${version} is built" >&2
echo "${directory}/src"
