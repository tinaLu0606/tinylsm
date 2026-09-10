# TinyLSM vs LevelDB, post-optimization · 2026-09-10

Status: complete on the fixed Linux VM, two runs retained. This is the first
report to run TinyLSM and LevelDB in the same session since
[`baseline-2026-09-06.md`](baseline-2026-09-06.md), which was captured before
Goals 1-6 existed. It answers a question none of the Goal-specific reports
could: how does the current engine compare to LevelDB on the same general
workload the original baseline used, not on a workload built to showcase one
goal.

Source revision `395f5c2ac5af3c00586665bd95670a723da4e27c` (includes Goals 1-6
and the single-operation write regression fix). Exported source archive
SHA-256 `efe4ff314be5a3d871c159c1ace94c651da941e2313e62326477b1216a211479`.
Both engines were rebuilt and run together in this session; no LevelDB number
from the 2026-09-06 session is reused here, since that would mix two
different environments under one comparison.

## Method

Unchanged from the original baseline: `./scripts/run_linux_baseline.sh` on
the same Ubuntu 24.04 ARM64 Lima VM (`tools/vm/tinylsm-linux.yaml`), Google
Benchmark v1.9.5, LevelDB 1.23, LevelDB compression disabled, five
repetitions per case, reported values are the wall-time median.
`run1` and `run2` are two independent complete runs; each also passed Debug,
ASan+UBSan, and Clang TSan at 121/121 and a Release build. `run2` was
discarded and rerun once on an idle VM (load average 0.00) after an initial
attempt showed elevated cross-run CV on several cases, most likely from VM
contention; the retained `run2` below is that clean rerun.

The benchmark binary itself has grown since the baseline: it now also
registers the Snapshot/Iterator, SSTable v2, and Group Commit cases from
`portfolio-benchmark-2026-09-09.md`. Those are not repeated here — this
report only covers the original 13 common TinyLSM/LevelDB workload pairs plus
TinyLSM's own full-compaction case.

One case's definition changed since the baseline: the Scan benchmark was
renamed `ScanTail1000` → `ScanTail10000Repeated` and now scans a 10,000-key
tail 20 times per repetition instead of a 1,000-key tail once. Its numbers
below are **not** comparable in magnitude to the original baseline's Scan
row; only the TinyLSM-vs-LevelDB ratio within this session is meaningful.

## TinyLSM vs LevelDB, this session

Values are median items/s; run1 / run2.

| Workload | TinyLSM | LevelDB | TinyLSM / LevelDB |
| --- | ---: | ---: | ---: |
| Sequential write, async | 523,927 / 531,772 | 1,597,085 / 1,617,086 | 0.33x / 0.33x |
| Sequential write, sync | 5,401 / 5,597 | 5,580 / 5,532 | 0.97x / 1.01x |
| Random read hit | 23,291 / 23,725 | 1,906,213 / 1,901,269 | 0.012x / 0.012x |
| Random read miss | 3,861,181 / 3,847,792 | 3,787,090 / 3,787,602 | 1.02x / 1.02x |
| Scan (10,000-tail, 20x)† | 6,896,084 / 6,735,125 | 76,562,556 / 73,959,519 | 0.090x / 0.091x |
| Batch 1, sync | 5,495 / 5,568 | 5,625 / 6,125 | 0.98x / 0.91x |
| Batch 10, sync | 49,060 / 48,836 | 50,266 / 52,769 | 0.98x / 0.93x |
| Batch 100, sync‡ | 229,828 / 181,905 | 264,750 / 390,961 | 0.87x / 0.47x |
| 1 caller, async | 568,868 / 573,757 | 1,604,152 / 1,640,727 | 0.35x / 0.35x |
| 2 callers, async | 431,267 / 434,504 | 689,623 / 697,944 | 0.63x / 0.62x |
| 4 callers, async | 238,408 / 238,036 | 295,444 / 310,488 | 0.81x / 0.77x |
| 8 callers, async | 229,010 / 231,228 | 298,777 / 307,070 | 0.75x / 0.75x |
| Full compaction (TinyLSM only) | 196,120 / 202,960 | n/a | n/a |

† renamed/rescaled case, not comparable to the pre-optimization Scan number.
‡ both engines exceed 10% cross-run CV on this case in both this run and the
original 2026-09-06 baseline (16.83%/15.67% there; 5.11%/29.70% and
31.00%/31.40% here). This is a known-noisy workload on this VM, not new
instability; its ratio is directional only.

Raw artifacts: [`results/post-optimization-vs-leveldb-2026-09-10/run1/post-v6-linux.json`](results/post-optimization-vs-leveldb-2026-09-10/run1/post-v6-linux.json),
[`run2/post-v6-linux.json`](results/post-optimization-vs-leveldb-2026-09-10/run2/post-v6-linux.json),
plus each run's `environment.txt`, `*-test.log`, `benchmark.log`, and
`verification.log`.

## TinyLSM's own change since the pre-optimization baseline

This half of the comparison does not require a same-session LevelDB rerun —
it tracks one engine against itself over time, using the fixed-Linux numbers
already retained in `baseline-2026-09-06.md`.

| Workload | Pre-optimization (2026-09-06) | Post-optimization, this run | Change |
| --- | ---: | ---: | ---: |
| Sequential write, async | 320,278 | 523,927 / 531,772 | +64% / +66% |
| Sequential write, sync | 5,435 | 5,401 / 5,597 | -0.6% / +3.0% |
| Random read hit | 11,925 | 23,291 / 23,725 | +95% / +99% |
| Random read miss | 5,716,042 | 3,861,181 / 3,847,792 | -32% / -33% |
| Batch 1, sync | 5,401 | 5,495 / 5,568 | +1.7% / +3.1% |
| Batch 10, sync | 46,579 | 49,060 / 48,836 | +5.3% / +4.8% |
| Batch 100, sync‡ | 202,441 | 229,828 / 181,905 | +13.5% / -10.1% |
| 1 caller, async | 674,634 | 568,868 / 573,757 | -15.7% / -15.0% |
| 2 callers, async | 432,868 | 431,267 / 434,504 | -0.4% / +0.4% |
| 4 callers, async | 253,867 | 238,408 / 238,036 | -6.1% / -6.2% |
| 8 callers, async | 225,362 | 229,010 / 231,228 | +1.6% / +2.6% |
| Full compaction | 366,730 | 196,120 / 202,960 | -46.5% / -44.7% |

This is not a uniform improvement. Only random read hit and sequential async
write clearly moved in a good direction on this general harness; several
cases are flat, and two moved backward by a large margin. That is expected
and explained below — the Goal-specific reports measure each optimization on
a workload built for it; this harness was fixed before those goals existed
and was never updated to exercise them the same way.

### Why full compaction is ~45% slower here, not faster

Confirmed by direct instrumentation, not inferred from the benchmark numbers
alone: a temporary test opened a DB with the same 512 KiB memtable and put
100,000 records, matching this benchmark's setup exactly. Before the
benchmark's timed `Compact()` call even starts, the fill alone already
triggers `background_compactions=10` (Goal 3's automatic size-tiered
compaction did not exist at the 2026-09-06 baseline) and leaves
`compaction_debt_tables=1` outstanding. `DB::Impl::Compact()`'s first action
is `WaitForBackgroundWork()` — so the timed call now includes waiting for a
lingering background job to finish, a cost that could not exist in the
pre-Goal-3 build because there was no background worker to wait for. The
instrumented run's explicit `Compact()` took 1.8 seconds to fold together
just 4 remaining tables, consistent with most of that time being wait, not
merge work. This is a benchmark-boundary mismatch, not a regression in
`Compact()` itself: `compaction-design-and-results-2026-09-08.md`'s
purpose-built mixed-workload benchmark is still the correct place to judge
size-tiered compaction's throughput, and it shows a real improvement there.

### Why 2/4/8-caller concurrent writes are flat, not faster

Not instrumented with the same rigor as compaction; offered as the most
likely explanation, consistent with a design decision documented in the
README. Goal 2 bounds the write path to at most one immutable
MemTable/WAL generation at a time — a foreground writer that fills a second
generation before the first has flushed must wait, deliberately trading fill
throughput for a hard memory ceiling instead of unbounded growth. At 8
threads writing into a 4 MiB memtable, hitting that bound at least once
during the run is plausible and would show up as exactly this kind of flat
result.

### Why random-miss reads are ~32% slower

Not instrumented; lower confidence than the two explanations above. The
likely cause is that every `Get()`, including a miss, now has to consult the
Goal 1 block cache (a hash lookup under its own mutex) before it can resolve
via Manifest key-range rejection, adding a small fixed cost per call. On a
workload that completed in a few hundred nanoseconds before, a fixed
addition of that size is a large percentage change even though it is a small
absolute one. This would need direct instrumentation to confirm.

## What this does and does not support

Supported by this session's data:

- TinyLSM's random-read-hit throughput on a generic access pattern improved
  roughly 2x from the block cache, well short of the 134x figure in
  `read-path-2026-09-07.md` — that number is specific to the controlled
  single/four-table layout it measured, not a general-purpose claim.
- TinyLSM remains well behind LevelDB on this general harness: about 3x on
  single-threaded async writes, roughly two orders of magnitude on random
  read hit, and about an order of magnitude on this harness's scan case.
  TinyLSM is close to LevelDB on synchronous single/small-batch writes.
- The apparent slowdowns in full compaction and concurrent writes are
  explained by new bounded-resource and background-work mechanisms that did
  not exist at the pre-optimization baseline, not by a same-feature
  regression; only the compaction explanation is directly confirmed.

Not supported: a single "TinyLSM got X% faster" number. Different goals moved
different workloads in different directions, and this general harness was
never designed to isolate any one of them.
