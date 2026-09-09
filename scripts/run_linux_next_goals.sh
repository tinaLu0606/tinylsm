#!/usr/bin/env bash

set -euo pipefail

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly project_root="$(cd -- "${script_dir}/.." && pwd)"

if [[ "$(uname -s)" != "Linux" ]]; then
  printf 'run_linux_next_goals.sh must run inside the fixed Linux VM\n' >&2
  exit 2
fi

readonly run_id="$(date -u +%Y%m%dT%H%M%SZ)"
readonly output_dir="${1:-${project_root}/build/linux-next-goals/${run_id}}"
readonly scope="${2:-all}"
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
  printf 'benchmark_scope=%s\n' "${scope}"
  uname -a
  free -h
  findmnt -no SOURCE,FSTYPE,OPTIONS --target "${project_root}"
  c++ --version
  clang++ --version
  cmake --version
  ninja --version
} >"${output_dir}/environment.txt" 2>&1

# Functional Debug/sanitizer evidence is recorded in the Goal devlog. This
# script intentionally performs only the Release build and two raw benchmark
# runs, so performance collection does not repeat unrelated validation.
case "${scope}" in
  all)
    readonly benchmark_filter='^TinyLSM/(Snapshot(MaterializedScan|Iterator|RetentionHeld|RetentionReleased|WriterOverlap)|SstableDataBlock(Encode|Lookup)|GroupCommit(On|Off))'
    ;;
  write-regression)
    readonly benchmark_filter='^TinyLSM/(SnapshotWriterOverlap(MaterializedScan|Iterator)|GroupCommit(On|Off))'
    ;;
  *)
    printf 'unknown benchmark scope: %s\n' "${scope}" >&2
    exit 2
    ;;
esac

cd "${project_root}"
./run release 2>&1 | tee "${output_dir}/release-build.log"
for repetition in run1 run2; do
  mkdir -p "${output_dir}/${repetition}"
  /usr/bin/time -v ./run benchmark -- \
    --benchmark_filter="${benchmark_filter}" \
    --benchmark_out="${output_dir}/${repetition}/next-goals.json" \
    --benchmark_out_format=json \
    --benchmark_report_aggregates_only=true \
    --benchmark_display_aggregates_only=true \
    --benchmark_enable_random_interleaving=true \
    2>&1 | tee "${output_dir}/${repetition}/benchmark.log"
done
python3 scripts/verify_next_goals_benchmark.py \
  "${output_dir}/run1/next-goals.json" "${output_dir}/run2/next-goals.json" \
  --scope "${scope}" \
  2>&1 | tee "${output_dir}/verification.log"

printf 'Linux Goals 4-6 %s performance artifacts: %s\n' "${scope}" "${output_dir}"
