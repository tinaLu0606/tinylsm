#!/usr/bin/env bash

set -euo pipefail

readonly TINYLSM_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

find_llvm_tool() {
  local tool="$1"
  if command -v "${tool}" >/dev/null 2>&1; then
    command -v "${tool}"
    return 0
  fi

  local homebrew_tool="/opt/homebrew/opt/llvm/bin/${tool}"
  if [[ -x "${homebrew_tool}" ]]; then
    printf '%s\n' "${homebrew_tool}"
    return 0
  fi

  return 1
}

run_command() {
  printf '+' >&2
  printf ' %q' "$@" >&2
  printf '\n' >&2
  "$@"
}
