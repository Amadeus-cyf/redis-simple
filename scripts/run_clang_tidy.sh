#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_root}/build/debug}"
source_regex="${SOURCE_REGEX:-^${repo_root}/(?!third_party|build).*}"

llvm_bin_dirs=(
  "/opt/homebrew/opt/llvm/bin"
  "/usr/local/opt/llvm/bin"
)

if command -v brew >/dev/null 2>&1; then
  llvm_prefix="$(brew --prefix llvm 2>/dev/null || true)"
  if [[ -n "${llvm_prefix}" ]]; then
    llvm_bin_dirs=("${llvm_prefix}/bin" "${llvm_bin_dirs[@]}")
  fi
fi

find_tool() {
  local tool_name="$1"
  local override="${2:-}"

  if [[ -n "${override}" ]]; then
    command -v "${override}" || return 1
    return 0
  fi

  if command -v "${tool_name}" >/dev/null 2>&1; then
    command -v "${tool_name}"
    return 0
  fi

  local bin_dir
  for bin_dir in "${llvm_bin_dirs[@]}"; do
    if [[ -x "${bin_dir}/${tool_name}" ]]; then
      printf '%s\n' "${bin_dir}/${tool_name}"
      return 0
    fi
  done

  return 1
}

clang_tidy_bin="$(find_tool clang-tidy "${CLANG_TIDY_BIN:-}")"
run_clang_tidy_bin="$(find_tool run-clang-tidy "${RUN_CLANG_TIDY_BIN:-}")"

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
  printf 'compile_commands.json not found in %s\n' "${build_dir}" >&2
  printf 'Run `cmake --preset debug` first, or set BUILD_DIR.\n' >&2
  exit 1
fi

# run-clang-tidy checks every file matching any positional regex, so the
# project-wide regex applies only when no files are named:
# `scripts/run_clang_tidy.sh server/db/db.cpp` checks just that file.
if [[ $# -eq 0 ]]; then
  set -- "${source_regex}"
fi

cd "${repo_root}"
exec "${run_clang_tidy_bin}" \
  -p "${build_dir}" \
  -clang-tidy-binary "${clang_tidy_bin}" \
  -config-file "${repo_root}/.clang-tidy" \
  -quiet \
  "$@"
