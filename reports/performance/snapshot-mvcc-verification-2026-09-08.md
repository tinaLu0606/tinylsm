# Snapshot and MVCC experiment record · 2026-09-08

Status: Goal 4 functional verification is complete. Fixed-Linux performance
experiments are intentionally deferred to a separately scheduled performance
task; this file makes no throughput, latency, or memory-performance claim.

## Deferred fixed-Linux experiment

When performance work is scheduled, run each configuration twice in the pinned
Ubuntu ARM64/ext4 VM and retain raw JSON, environment capture, median, and
coefficient of variation:

| Experiment | Variables | Required measures |
| --- | --- | --- |
| Iterator vs Scan | 10k, 100k, 1m entries | first-result latency, full time, peak RSS |
| Writer overlap | materialized Scan vs Iterator; concurrent 5,000 async writes | writer p50/p95/p99 and exclusive-lock wait |
| Snapshot retention | equivalent 0/10/60-second overwrite windows | retained versions/bytes and live SST bytes |
| GC | compact before/after oldest Snapshot release | reclaimed versions/bytes and read correctness |

The implementation exposes `GetSnapshotMetrics()` for active Snapshot count,
oldest sequence, and retained full-compaction entries/bytes. It does not turn
these counters into a throughput claim.

The benchmark registers all three scan sizes, reports Linux `VmRSS` while the
result object/iterator is still live, and uses 0/1/6 full overwrite generations
as deterministic equivalents of 0/10/60 retention windows. It also includes
two controlled writer-overlap cases. The latter measures writer latency and
lock wait; it must not be interpreted as proof that `Scan` holds the lock for
its entire traversal, because both `Scan` and `Iterator` release it after
capturing their sources. Retention cases emit `live_sstable_bytes` together
with the Snapshot retention counters.

## Local functional evidence

One Debug, ASan/UBSan, and TSan milestone run on 2026-09-08 each passed 117/117
tests after adding public Snapshot/Iterator integration coverage, a fixed-seed
MVCC reference model across Put/Delete/WriteBatch/Flush/Compaction,
cross-DB and reopen Snapshot rejection, release-driven MVCC reclamation,
multi-version SSTable coverage, visibility filtering, compaction-failure
behavior, and concurrent writer/flush/compaction behavior. Subsequent changes
follow the plan's risk-based verification rule: run only affected tests, and
rerun a sanitizer only when its corresponding ownership or concurrency risk
changes.
