# Goal 2 · Bounded asynchronous write path

This report records the completed asynchronous-write extension at source revision
`32c950b`. The fixed Linux runs identify an exported source snapshot with SHA-256
`c856985971b906a5f8181ad9e9cf3484267db9d7825babacad79ee717d4ac9a0`.

## Design and durable states

Each DB has exactly one active MemTable/WAL and at most one immutable
MemTable/WAL. When active memory reaches its threshold, TinyLSM creates and
syncs a replacement active WAL, then publishes a Manifest v3 with the old WAL
as immutable. The foreground can now accept a new active generation. A single
worker builds and validates an SSTable from the immutable generation, publishes
the SSTable and removal of the immutable WAL in a second Manifest commit, then
releases memory and best-effort removes the old WAL file.

```text
active WAL + active MemTable
        | threshold
        v
Manifest v3 {immutable=old, active=new}  <- durable rotation
        |                                      (one immutable only)
        v
background SST build / validate / rename / SyncDir
        v
Manifest v3 {immutable=none, add SST}    <- durable flush
        v
release immutable memory + best-effort WAL cleanup
```

If another active MemTable fills before that second commit, the writer waits;
the queue depth is consequently bounded at one. A worker failure is sticky for
future data operations and `Close`. A Manifest rename followed by failed
directory sync makes the current handle terminal until close/reopen. Recovery
reads Manifest v1/v2/v3, replays an optional immutable WAL first and then the
active WAL while enforcing the published sequence floor and increasing sequence
order. Version-1 and version-2 WAL records remain readable.

## Measurement method

The benchmark uses 256-byte values, a 256 KiB MemTable threshold, five manual
repetitions, and an empty temporary database per repetition. Async writes use
100,000 `sync_on_write=false` puts; sync writes use 20,000
`sync_on_write=true` puts. The unequal counts make both measurements long enough
to cover many rotations without exceeding the current explicit-compaction
prototype's retained-SSTable file-handle limit; compare their per-operation
throughput and Put latency, not total elapsed work.

`items_per_second` and Put p50/p95/p99 cover only the put loop. The counters
are read after `Close`, so `background_flushes` includes the final close drain;
`flush_stall_ns` is foreground backpressure accumulated during puts. RSS is
`/usr/bin/time -v` maximum resident set size for the `./run benchmark` wrapper,
so it includes process/build-wrapper effects and is reported as an observation,
not a tight engine-memory bound.

The fixed guest is Ubuntu 24.04 on Lima ARM64: Linux 6.8.0-134, four vCPU,
7.7 GiB RAM, ext4 `/dev/vda1`, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, and
Ninja 1.11.1. Exact environment captures and raw Google Benchmark JSON are
stored beside this report.

## Fixed-Linux results

| Case | Run 1 median | Run 2 median | Put p50 / p95 / p99 (us), run 1 / run 2 | Stall, run 1 / run 2 |
| --- | ---: | ---: | --- | --- |
| async | 123,050 /s | 127,375 /s | 2.460 / 5.294 / 6.525; 2.466 / 5.274 / 6.327 | 382.064 / 362.846 ms |
| sync | 4,775 /s | 4,773 /s | 184.517 / 218.366 / 275.763; 183.283 / 217.234 / 279.308 | 45.441 / 50.731 ms |

The async/sync throughput ratio is 25.8x in run 1 and 26.7x in run 2. Async
performed 134 rotations/flushes with queue-depth maximum one and zero per-write
WAL syncs; sync performed 26 rotations/flushes and 20,000 WAL syncs. Throughput
CV is 3.51%/2.50% for async and 5.50%/2.72% for sync, below 10% in both retained
runs. The logged maximum RSS is 333,572 KiB in run 1 and 34,304 KiB in run 2;
the large cold/warm spread is why RSS is not used as an engine-memory claim.

The earlier short 10,000/2,000-operation Linux samples had async/sync throughput
CV above 10%; they are intentionally not used here. Increasing the fixed work
while keeping the queue bounded produced the retained stable runs.

Raw artifacts:

- `results/goal2-write-linux-2026-09-07-run1.json`
- `results/goal2-write-linux-2026-09-07-run2.json`
- `results/goal2-write-linux-2026-09-07-run1-environment.txt`
- `results/goal2-write-linux-2026-09-07-run2-environment.txt`
- `results/goal2-write-linux-2026-09-07-run1-benchmark.log`
- `results/goal2-write-linux-2026-09-07-run2-benchmark.log`

The matching macOS raw JSON remains in
`results/goal2-write-macos-2026-09-07-run1.json` and `run2.json`; it is a local
observation, not the durability-performance reference.

## Correctness and failure boundary

`AsyncFlushRecoveryTest.BackgroundFailureIsStickyAndImmutableWalRecovers`
injects an SST temporary-file failure after rotation, verifies that the worker
error becomes sticky, destroys the handle, then reopens and recovers the
immutable WAL. Existing flush-recovery tests cover WAL rotation, temporary SST
build/rename, Manifest rename/SyncDir, cleanup retry, terminal close/reopen,
and both old-WAL cleanup failure/exception behavior. The test suite also covers
version-1 Manifest compatibility and version-1/version-2 WAL replay.

The final local evidence is Debug 102/102, ASan/UBSan 102/102, Clang TSan
102/102 with no race report, and a Release build. Each retained fixed-Linux run
again passed Debug, ASan/UBSan, and Clang TSan at 102/102, built Release, and
validated its JSON with `scripts/verify_write_benchmark_json.cmake`.

## Group-commit decision and limits

At the time of this 2026-09-07 experiment, group commit was deliberately not
implemented. The workload establishes the per-write sync cost, but it does not
profile a bounded multiwriter queue or prove a batching performance benefit.
Goal 6 subsequently implemented the bounded queue and its functional contract;
this historical experiment still provides no throughput or latency claim for
that implementation. See `docs/plans/next-engineering-goals.md` for its
separately scheduled multiwriter experiment.

This is still a learning prototype: live SSTable readers retain file handles
until explicit full compaction or close. The benchmark's 134 live tables stays
within the tested descriptor budget; table-set management and automatic
compaction remain Goal 3 work.
