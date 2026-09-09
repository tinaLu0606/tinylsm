# Post-optimization TinyLSM vs LevelDB rerun · 2026-09-09

Status: prepared, not yet run. No new harness or code is required; this reuses
the existing baseline benchmark unchanged.

## Question this answers

`reports/performance/baseline-2026-09-06.md` is the only report that ever ran
TinyLSM and LevelDB in the same session. It is a **pre-optimization** snapshot.
Every later report (`read-path-2026-09-07.md`, `write-path-2026-09-07.md`,
`compaction-design-and-results-2026-09-08.md`,
`portfolio-benchmark-2026-09-09.md`) only compares TinyLSM against itself,
because their harnesses register TinyLSM-only cases (for example "four-table
oldest hit", which has no equivalent LevelDB API call). None of them mention
LevelDB.

So there is currently no evidence for "how does the optimized engine compare
to LevelDB", only "how much did TinyLSM improve internally per goal". Chaining
the old LevelDB numbers to the new TinyLSM numbers would compare two different
sessions, days apart, on process-level noise that the project's own baseline
report explicitly warns against ("do not interpret ... as a durability-adjusted
ranking"). This plan produces a real same-session number instead of an
estimate.

## What to run

No new benchmark code: `scripts/run_linux_baseline.sh` already builds the
`benchmark` CMake preset with `TINYLSM_BENCHMARK_LEVELDB=ON` and runs both
engines through the same 25-case suite (sequential write async/sync, random
read hit/miss, scan of the final 1,000 keys, sync batches of 1/10/100, 1/2/4/8
concurrent callers, and TinyLSM full compaction) that produced the original
baseline table. Rerunning it at the current revision is sufficient.

## Precondition

The current worktree has the Goal 4-6 implementation and the MemTable
`ApplyBatch()` regression fix uncommitted. "Post-optimization" must describe a
committed, pushed revision, not a local tree — commit and push first, then
record that commit hash as `source_revision` for this run (the script does
this automatically via `git rev-parse HEAD` when run from a clean checkout).

## Procedure

1. Confirm the fixed Lima VM (`tools/vm/tinylsm-linux.yaml`) is up and on its
   guest-local ext4 disk, matching every prior run.
2. Copy the committed revision into the VM (clean checkout or exported
   archive with `TINYLSM_SOURCE_ARCHIVE_SHA256` set, same as prior runs).
3. Run `./scripts/run_linux_baseline.sh <output-dir>` twice, exactly as the
   original baseline did, and retain both runs' `environment.txt`,
   `debug-test.log`, `asan-ubsan-test.log`, `tsan-test.log`,
   `release-build.log`, `benchmark.log`, raw JSON, and `verification.log`.
4. `scripts/verify_benchmark_json.cmake` must accept all 25 expected medians
   in each run; if any case's CV exceeds 10%, rerun on an idle VM before
   treating it as a formal result, per the project's existing acceptance rule.
5. Do **not** reuse the LevelDB numbers from `pre-v6-macos-2026-09-06.json` /
   `post-v6-linux-2026-09-07.json`. Both engines must come from this same new
   session so the comparison isolates engine behavior rather than
   environment drift across sessions.

## Comparison to assemble afterward

Build a three-column table per workload: pre-optimization TinyLSM (from the
retained fixed-Linux table in `baseline-2026-09-06.md`), post-optimization
TinyLSM (this run), and LevelDB (this run, same session). Keep each number's
CV alongside it, exactly as the existing report does.

## Interpretation boundaries to state up front

This harness's case set was fixed before Goals 1-6 existed, so it does not
exercise every optimization equally:

- Random read hit is the case most likely to show the Goal 1 block-cache
  effect, but this harness's keys are not laid out to reproduce Goal 1's
  controlled single/four-table scenarios; expect a real but smaller
  improvement than the 134x figure in `read-path-2026-09-07.md`, which used a
  purpose-built layout.
- Sequential write async/batch-sync cases are the ones Goal 2 (bounded async
  flush) and Goal 6 (group commit) should move; single-caller sequential
  write is not a multiwriter workload, so Goal 6's gain may not show here.
- Full compaction is TinyLSM-only in this harness; there is still no
  LevelDB-comparable compaction number.
- The harness's 16-byte keys are close to worst-case for SSTable v2's
  prefix compression (Goal 5), which needs shared key prefixes to help; do
  not expect this run to demonstrate that goal's benefit at all.
- Scan and random-miss are not targeted by any goal and should be read as a
  stability check, not an improvement claim.

State these boundaries in the resulting report before presenting any numbers,
the same way `performance-extension-closeout-2026-09-08.md` scopes its
Goal 1-3 claims.

## Deliverable

A new report, `reports/performance/post-optimization-vs-leveldb-<run-date>.md`,
structured like `baseline-2026-09-06.md`'s fixed-Linux table plus the
comparison and caveats above. This is supporting evidence for a resume-facing
claim, not a blocking requirement for closing any Goal.
