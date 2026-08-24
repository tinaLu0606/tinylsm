#!/usr/bin/env bash

set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_command git

readonly clang_tidy="${TINYLSM_CLANG_TIDY:-$(find_llvm_tool clang-tidy || true)}"
[[ -n "${clang_tidy}" ]] || die "clang-tidy not found in PATH or Homebrew LLVM"

readonly preset="${1:-dev-debug}"
readonly build_dir="${TINYLSM_ROOT}/build/${preset}"
readonly compilation_database="${build_dir}/compile_commands.json"

if [[ ! -f "${compilation_database}" ]]; then
  die "missing ${compilation_database}; configure after V0 adds the first compilation target"
fi

files=()
while IFS= read -r -d '' file; do
  files+=("${TINYLSM_ROOT}/${file}")
done < <(git -C "${TINYLSM_ROOT}" ls-files -z -- '*.c' '*.cc' '*.cpp')

if [[ ${#files[@]} -eq 0 ]]; then
  printf 'No tracked C/C++ translation units to lint.\n'
  exit 0
fi

run_command "${clang_tidy}" -p "${build_dir}" "${files[@]}"
