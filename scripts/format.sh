#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:-format}"
readonly clang_format_major=22

if [[ "${mode}" != "format" && "${mode}" != "--check" ]]; then
  printf 'usage: %s [--check]\n' "$0" >&2
  exit 2
fi

clang_format_candidates=(
  clang-format-22
  /opt/homebrew/opt/llvm/bin/clang-format
  /usr/local/opt/llvm/bin/clang-format
  /opt/homebrew/opt/llvm@22/bin/clang-format
  /usr/local/opt/llvm@22/bin/clang-format
  clang-format
)
if command -v brew >/dev/null 2>&1; then
  for formula in llvm llvm@22; do
    llvm_prefix="$(brew --prefix "${formula}" 2>/dev/null || true)"
    if [[ -n "${llvm_prefix}" ]]; then
      clang_format_candidates=(
        "${llvm_prefix}/bin/clang-format"
        "${clang_format_candidates[@]}"
      )
    fi
  done
fi
if [[ -n "${CLANG_FORMAT_BIN:-}" ]]; then
  clang_format_candidates=("${CLANG_FORMAT_BIN}")
fi

clang_format_bin=""
for candidate in "${clang_format_candidates[@]}"; do
  candidate_path="$(command -v "${candidate}" 2>/dev/null || true)"
  if [[ -z "${candidate_path}" ]]; then
    continue
  fi
  version="$(${candidate_path} --version 2>/dev/null || true)"
  if [[ "${version}" =~ version[[:space:]]+${clang_format_major}\. ]]; then
    clang_format_bin="${candidate_path}"
    break
  fi
done

if [[ -z "${clang_format_bin}" ]]; then
  printf 'clang-format %s is required. Install llvm or set CLANG_FORMAT_BIN.\n' \
    "${clang_format_major}" >&2
  exit 1
fi

cd "${repo_root}"
files=()
while IFS= read -r file; do
  if [[ -f "${file}" ]]; then
    files+=("${file}")
  fi
done < <(git ls-files '*.h' '*.cpp' ':!:third_party/**' ':!:build/**')

if (( ${#files[@]} == 0 )); then
  exit 0
fi

if [[ "${mode}" == "--check" ]]; then
  "${clang_format_bin}" --dry-run --Werror "${files[@]}"
else
  "${clang_format_bin}" -i "${files[@]}"
fi
