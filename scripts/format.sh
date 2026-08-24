#!/usr/bin/env bash

set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_command git

readonly clang_format="${TINYLSM_CLANG_FORMAT:-$(find_llvm_tool clang-format || true)}"
[[ -n "${clang_format}" ]] || die "clang-format not found in PATH or Homebrew LLVM"

files=()
while IFS= read -r -d '' file; do
  files+=("${TINYLSM_ROOT}/${file}")
done < <(git -C "${TINYLSM_ROOT}" ls-files -z -- '*.c' '*.cc' '*.cpp' '*.h' '*.hpp')

if [[ ${#files[@]} -eq 0 ]]; then
  printf 'No tracked C/C++ files to format.\n'
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  run_command "${clang_format}" --dry-run --Werror "${files[@]}"
else
  run_command "${clang_format}" -i "${files[@]}"
fi
