# Goals 4-6 performance experiment contract · 2026-09-09

Status: completed on the fixed Linux VM. Both retained full runs contain all 38
median cases and the cross-run real-time CV verifier accepted every case. The
single-operation write regression found by that run was repaired, then all 12
affected cases passed a two-run targeted verification. Detailed results and
limitations are recorded in
[`reports/performance/portfolio-benchmark-2026-09-09.md`](../../../reports/performance/portfolio-benchmark-2026-09-09.md).

## Fixed environment and reproducibility

Run `scripts/run_linux_next_goals.sh <output-dir> [all|write-regression]` inside
the pinned Ubuntu ARM64/ext4 VM. The runner records the source revision or
archive hash, dirty status, toolchain, filesystem mount, memory, and two
independent raw JSON runs.
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
bounded batches of at most 500,000 operations so setup cost remains bounded and
does not dominate the run; the measured Scan, Iterator, and overlap work is
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

## Retained result

The original full-run artifacts are under
[`reports/performance/results/goal4-6-linux-2026-09-09/`](../../../reports/performance/results/goal4-6-linux-2026-09-09/).
The exported source archive SHA-256 is
`ef480687a6727112946caf6d956faf781e44f2d7b433b1aabfa3ed7e7a4619c0`.
All 38 cross-run real-time median CV values are at most 3.645%.

That run found a severe single-operation write regression caused by full
MemTable copying in `ApplyBatch()`. The implementation now stages only touched
keys before an allocation-free commit. The retained targeted rerun is under
[`reports/performance/results/write-regression-fix-linux-2026-09-09/`](../../../reports/performance/results/write-regression-fix-linux-2026-09-09/);
its source archive SHA-256 is
`d9f631362c1b6fde4d9ffcb15b0a251c891f3cc9e81c297e3b50f7859640cfe6`.
Both runs contain all 12 affected median cases and their cross-run real-time CV
values are at most 9.309%. No unaffected Snapshot-retention or SSTable-codec
case was repeated.
