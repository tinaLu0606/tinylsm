#!/usr/bin/env bash

set -euo pipefail

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly project_root="$(cd -- "${script_dir}/.." && pwd)"

if [[ "$(uname -s)" != "Linux" ]]; then
  printf 'run_linux_compaction_goal3.sh must run inside the fixed Linux VM\n' >&2
  exit 2
fi

readonly run_id="$(date -u +%Y%m%dT%H%M%SZ)"
readonly output_dir="${1:-${project_root}/build/linux-compaction-goal3/${run_id}}"
mkdir -p "${output_dir}"

{
  printf 'captured_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  if git -C "${project_root}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    printf 'source_revision=%s\n' "$(git -C "${project_root}" rev-parse HEAD)"
    printf 'source_status_begin\n'
    git -C "${project_root}" status --short
    printf 'source_status_end\n'
  else
    printf 'source_revision=%s\n' "${TINYLSM_SOURCE_REVISION:-snapshot-without-git-metadata}"
    printf 'source_status=unavailable-in-exported-snapshot\n'
  fi
  if [[ -n "${TINYLSM_SOURCE_ARCHIVE_SHA256:-}" ]]; then
    printf 'source_archive_sha256=%s\n' "${TINYLSM_SOURCE_ARCHIVE_SHA256}"
  fi
  uname -a
  free -h
  findmnt -no SOURCE,FSTYPE,OPTIONS --target "${project_root}"
  c++ --version
  clang++ --version
  cmake --version
  ninja --version
} >"${output_dir}/environment.txt" 2>&1

readonly benchmark_filter='^TinyLSM/CompactionMixed(Manual|SizeTiered)/'

cd "${project_root}"
./run test 2>&1 | tee "${output_dir}/debug-test.log"
./run asan 2>&1 | tee "${output_dir}/asan-ubsan-test.log"
CC=clang CXX=clang++ ./run tsan 2>&1 | tee "${output_dir}/tsan-test.log"
./run release 2>&1 | tee "${output_dir}/release-build.log"
/usr/bin/time -v ./run benchmark -- \
  --benchmark_filter="${benchmark_filter}" \
  --benchmark_out="${output_dir}/compaction-goal3.json" \
  --benchmark_out_format=json \
  --benchmark_report_aggregates_only=true \
  --benchmark_display_aggregates_only=true \
  --benchmark_enable_random_interleaving=true \
  2>&1 | tee "${output_dir}/benchmark.log"

cmake -DINPUT="${output_dir}/compaction-goal3.json" \
  -P scripts/verify_compaction_benchmark_json.cmake \
  2>&1 | tee "${output_dir}/verification.log"

printf 'Linux Goal 3 artifacts: %s\n' "${output_dir}"
