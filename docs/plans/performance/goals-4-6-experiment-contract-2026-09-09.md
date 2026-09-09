# Goals 4-6 performance experiment contract · 2026-09-09

Status: prepared locally, not yet run on the fixed Linux VM. This document
defines the only benchmark matrix that may be used for formal performance
claims about Goals 4-6.

## Fixed environment and reproducibility

Run `scripts/run_linux_next_goals.sh <output-dir>` inside the pinned Ubuntu
ARM64/ext4 VM. The runner records the source revision or archive hash, dirty
status, toolchain, filesystem mount, memory, and two independent raw JSON runs.
It performs a Release build only: functional Debug/sanitizer evidence is already
recorded in the devlog and must not be rerun for each performance repetition.

`scripts/verify_next_goals_benchmark.py` requires every expected median from
each raw JSON and computes CV from the two medians. A result with any real-time
CV above 10% is retained as an artifact but rejected as a formal conclusion.

## Goal 4: Snapshot and Iterator

The prepared runner retains the 12 existing cases:

- Materialized `Scan` versus pull Iterator at 10k, 100k, and 1m entries.
- Held/released Snapshot retention at deterministic 0/10/60-second-equivalent
  overwrite windows.
- Writer overlap with materialized Scan versus Iterator.

Database population is outside the manually timed traversal interval. It uses
bounded batches of at most 500,000 operations so setup does not trigger the
quadratic full-MemTable copy cost of submitting every prepared key as a
one-operation `WriteBatch`; the measured Scan, Iterator, and overlap work is
unchanged.

They report operation time, iterator/Scan behavior, Snapshot retained
versions/bytes, live SST bytes, writer p50/p95/p99, and lock wait. They do not
claim that either traversal holds the DB state lock for its whole lifetime.

## Goal 5: SSTable v2 format

The 16 data-block microbenchmarks use fixed 100-byte values and two key shapes:
shared tenant/collection prefixes and sorted pseudo-random keys. For each shape
they measure v1 plus v2 restart intervals 4, 16, and 64.

- `SstableDataBlockEncode*` measures codec CPU and reports `encoded_bytes` and
  `bytes_per_entry`.
- `SstableDataBlockLookup*` measures a random hit in a representative 128-entry
  data block and reports `block_bytes` and `bytes_per_entry`.

The v1 lookup deliberately uses the compatible full-block decode path; v2 uses
the restart-aware `FindDataBlockEntry` path. These are format microbenchmarks,
not a claim about whole-DB Open time, cache behavior, or all-SSTable Scan.

## Goal 6: bounded Group Commit

The 10 sync-write cases use 1/2/4/8/16 writers with a fixed total of 8,192
256-byte writes per case. `GroupCommitOff*` keeps the same bounded queue but
sets `max_group_commit_requests = 1`; `GroupCommitOn*` sets it to 8. This is a
controlled physical-WAL-group comparison, not a comparison to the pre-Goal-6
implementation.

Each case records operations/s, p50/p95/p99 Put latency, physical groups,
`wal_syncs`, `wal_syncs_per_write`, cumulative queue wait, and maximum queue
depth. The expected conclusion is conditional: only the retained two-run Linux
data can establish whether grouping improved throughput or tail latency.

## Before running

- Copy an exact source archive into the VM and export its SHA-256 as
  `TINYLSM_SOURCE_ARCHIVE_SHA256`, or run a committed clean checkout.
- Keep the VM on its ext4 disk, not a host shared filesystem.
- Do not compare macOS smoke values with Linux results.
- Preserve both raw JSON files, `environment.txt`, Release build log, benchmark
  logs, and `verification.log` under the selected output directory.
