# TinyLSM

TinyLSM is a compact C++20 key-value storage engine built to explore the core
mechanics of an LSM tree: write-ahead logging, ordered in-memory state, immutable
SSTables, manifest-based recovery, tombstones, checksums, and crash-aware file
publication.

The project is intentionally small enough to inspect end to end. It is a
learning-oriented prototype rather than a production database. The write path
uses an active WAL/MemTable plus one bounded immutable WAL/MemTable generation:
rotation is durable before a single background worker flushes the immutable
generation into an ordered set of SSTables. Range scans lazily merge active
memory, immutable memory, and tables; callers can explicitly run synchronous
full compaction. `Get`, `Scan`, and diagnostic snapshots share a read lock;
writes, compaction, and close coordinate state with the exclusive lock, so a
long Scan can delay a writer.

## Quick start

### Requirements

- CMake 3.24 or newer
- Ninja
- A C++20 compiler
- GoogleTest for normal test builds; the sanitizer preset fetches a pinned
  source copy so the test framework receives the same instrumentation
- Protobuf (if it is not installed, CMake fetches the pinned v29.3 source)
- Node.js and npm when developing the optional Lab UI

On macOS, the main build dependencies can be installed with Homebrew:

```sh
brew install cmake ninja googletest protobuf
```

Build the project and run the Debug test suite:

```sh
./run test
```

Run the sanitizer configuration:

```sh
./run asan
./run tsan
```

`run` is the repository's Bash entry point. It translates short developer
commands into the corresponding CMake, build, test, formatting, or lint command.
Use `./run help` to see the current command list.

## CLI usage

The CLI opens the database for one operation and closes it before returning. The
database directory is created automatically when it does not exist.

```sh
./run cli -- /tmp/tinylsm-demo put name Tina
./run cli -- /tmp/tinylsm-demo get name
./run cli -- /tmp/tinylsm-demo put city Singapore
./run cli -- /tmp/tinylsm-demo scan
./run cli -- /tmp/tinylsm-demo delete name
./run cli -- /tmp/tinylsm-demo get name
```

`scan` accepts an optional half-open key range, `[begin, end)`:

```sh
./run cli -- /tmp/tinylsm-demo scan a n
```

For machine-readable `get` and `scan` output, add `--json`:

```sh
./run cli -- /tmp/tinylsm-demo get city --json
./run cli -- /tmp/tinylsm-demo scan --json
```

JSON output is JSON Lines. Keys and values are Base64 encoded so arbitrary bytes
can be represented without escaping or encoding ambiguity. Command-line
arguments themselves remain subject to shell limitations; binary applications
should use the C++ API.

## TinyLSM Lab

The experimental React and TypeScript frontend is maintained as the
`tools/lab_web` Git submodule. A fresh checkout can build and start the local
Lab with one command:

```sh
git submodule update --init tools/lab_web
./run lab
```

Then open the URL printed by the server (by default
`http://127.0.0.1:8080`). `./run lab` installs the frontend dependencies when
needed, builds the frontend with the live transport selected, builds the C++
server, and serves the packaged UI and API from that one loopback process.
Pass server options after `--`, for example `./run lab -- --port 18080`.

For frontend-only work, start Vite separately:

```sh
cd tools/lab_web
VITE_LAB_API=live npm run dev
```

`tinylsm_lab_server` links TinyLSM directly; it never shells out to the CLI.
It accepts Open, Close, Reopen, Put, Get, Delete, Scan and Compact on one
serialized session, exposes copied internal diagnostic snapshots, lists real
directory files, retains the latest 10,000 detailed operation/event records,
and serves SSE events with `Last-Event-ID` replay. Storage inspection reads
Manifest, WAL records, SSTable blocks, and hexadecimal file ranges through
bounded, paged APIs. The server also records bounded latency/resource series,
rotates detailed session event logs as JSONL, and runs deterministic seeded
workloads against a server-side ordered reference map. High-rate workloads keep
only aggregates and a first failure sample; they do not persist every operation.
The Recovery workspace first copies the currently open database into a
server-created temporary sandbox. Its three scenarios exercise a no-`Close()`
worker exit, a one-byte WAL-tail truncation, and Manifest CRC corruption. Only
the sandbox is mutated; preview, run, reset, and audit events are all exposed
by the API. Recovery paths are never accepted from the browser.
It only listens on `127.0.0.1` (or `::1` when embedded) and limits JSON request
bodies to 8 MiB. `cpp-httplib` v0.18.0 and `nlohmann/json` v3.11.3 are pinned
through CMake FetchContent for this local tool.

Pass `--static-dir tools/lab_web/dist` after `npm run build` to have the same
server also serve the packaged frontend; Vite remains useful for live frontend
development.

Live data is labelled `Live`; it is never filled from Mock fixtures. See
`tools/lab_web/README.md` for the frontend transport boundary and verification
steps.

## Developer commands

| Command | Purpose |
| --- | --- |
| `./run configure [preset]` | Generate build files without compiling |
| `./run build [preset]` | Configure and compile the selected preset |
| `./run test [preset]` | Configure, compile, and run CTest |
| `./run cli -- <args>` | Build and run `tinylsm_cli` |
| `./run lab [-- <server args>]` | Build the live Lab UI and serve it with the loopback Lab API |
| `./run format [--check]` | Apply or verify `clang-format` |
| `./run lint [preset]` | Run `clang-tidy` with the preset compilation database |
| `./run asan` | Build and test with AddressSanitizer and UBSan |
| `./run tsan` | Build and test with ThreadSanitizer |
| `./run release` | Build the Release preset |
| `./run benchmark [-- <args>]` | Build and run the pinned Release benchmark suite |
| `./run clean [preset]` | Clean the selected build tree |

Available CMake presets are:

- `dev-debug`: Debug build with tools and tests.
- `dev-asan-ubsan`: Debug build with AddressSanitizer and UndefinedBehaviorSanitizer.
- `dev-tsan`: Debug build with ThreadSanitizer.
- `release`: optimized build without the test targets.
- `benchmark`: optimized benchmark build with pinned Google Benchmark and,
  by default, pinned LevelDB comparison targets.

The `package` command is reserved for future install/CPack support and currently
reports that packaging is not implemented.

## Public C++ API

The public interface is deliberately small: `Open`, `Put`, `Get`, `Delete`,
atomic `Write`, `Scan`, `Compact`, and `Close`. Expected storage failures are
returned through `Status` and `Result<T>` instead of exceptions.

```cpp
#include <iostream>
#include <utility>

#include "tinylsm/db.h"

int main() {
  auto opened = tinylsm::DB::Open("/tmp/tinylsm-api-demo");
  if (!opened.ok()) {
    std::cerr << opened.status().ToString() << '\n';
    return 1;
  }

  auto db = std::move(opened.value());

  auto status = db->Put("language", "C++");
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }

  auto value = db->Get("language");
  if (value.ok())
    std::cout << value.value() << '\n';

  status = db->Close();
  return status.ok() ? 0 : 1;
}
```

Keys and values are byte strings. Database handles are movable but non-copyable.
`Get`, `Scan`, and diagnostic snapshots share a read lock; writes, compaction,
and close use the exclusive state boundary. Moving or destroying a handle still
requires exclusive ownership.

`WriteBatch` groups several ordered `Put`/`Delete` operations into one WAL
record and one optional WAL sync. Recovery applies a complete record in full
and discards an incomplete final record in full, so a crash cannot expose half
of the batch. This is atomicity, not isolation across multiple DB handles and
not rollback-capable transactions.

```cpp
tinylsm::WriteBatch batch;
batch.Put("account:a", "90");
batch.Put("account:b", "110");
auto status = db->Write(batch);
```

`GetWriteMetrics()` returns cumulative, point-in-time write-path counters for
one handle: WAL syncs, MemTable rotations, successful/failed background flushes,
backpressure waits and duration, plus current and maximum immutable-generation
queue depth/bytes. It is an observability aid, not a latency benchmark by itself.

## Implemented scope

- WAL-first `Put`, `Delete`, and atomic `WriteBatch`, with optional per-write or
  per-batch synchronization, active/immutable WAL rotation, and one bounded
  background MemTable flush worker.
- An ordered MemTable that keeps the newest sequence for each user key.
- Immutable, block-indexed SSTables with CRC32C integrity checks and complete
  validation of Manifest-referenced table data during startup.
- Tombstones that hide deleted values across memory and disk.
- Half-open ordered range scans that merge active MemTable, immutable MemTable,
  and SSTable state.
- Explicit synchronous full compaction into zero or one replacement SSTable.
- A version-3 Manifest snapshot that records active and optional immutable WALs,
  the ordered SSTable set, and sequence state while retaining version-1 and
  version-2 read compatibility.
- Startup recovery through Manifest loading, immutable-WAL replay, then
  active-WAL replay.
- Conservative orphan cleanup for canonical numbered WAL/SSTable names after
  successful recovery and at later maintenance checkpoints.
- Detection of malformed records, truncated data, checksum failures, invalid
  ordering, missing files, and size mismatches.
- Serialized public DB operations with concurrent integration coverage.
- Unit, integration, CLI, fault-injection, and recovery tests, plus ASan/UBSan
  and TSan test builds.
- A Release benchmark suite with fixed workloads and a pinned LevelDB adapter.

## Architecture

```text
                 +-------------------------------+
                 |  C++ API / tinylsm_cli        |
                 +---------------+---------------+
                                 |
                                 v
                 +-------------------------------+
                 |  DB facade + DB::Impl         |
                 |  validation and orchestration |
                 +-----+-----------+-------------+
                       |           |
              writes   |           | reads / scans
                       v           v
                 +-----------+  +-----------+
                 |    WAL    |  | MemTable  |
                 | append +  |  | ordered   |
                 | replay    |  | latest key|
                 +-----------+  +-----+-----+
                                        |
                                        | flush
                                        v
                                  +-----------+
                                  | SSTables  |
                                  | blocks +  |
                                  | indexes   |
                                  +-----------+

                 +-------------------------------+
                 | Manifest                      |
                 | authoritative file metadata   |
                 +-------------------------------+

                 All persistent I/O passes through
                 the FileSystem abstraction.
```

The main modules are:

- `include/tinylsm`: stable public types and database API.
- `src/db`: operation orchestration and lifecycle management.
- `src/memtable`: ordered in-memory key state.
- `src/wal`: WAL record codec, writer, and recovery reader.
- `src/sstable`: immutable table builder, reader, block index, and format.
- `src/manifest`: persistent metadata codec and publication protocol.
- `src/io`: filesystem interfaces and POSIX implementation.
- `src/iterator`: internal merge iteration shared by scans and compaction.
- `tools/cli`: argument parsing and CLI command handlers.
- `tools/lab_web`: experimental Lab UI maintained as a Git submodule.

## Core data flows

### Write path

```text
Put/Delete or WriteBatch
    -> validate sizes and allocate a sequence number
    -> append one complete record to WAL
    -> optionally fsync WAL
    -> apply one entry or the complete batch to MemTable
    -> when full, create next active WAL and durably publish
       {active WAL, one immutable WAL} in Manifest v3
    -> move old MemTable/WAL to the one-item immutable generation
    -> one background worker builds and verifies its SSTable
    -> durably publish the SSTable and clear immutable WAL from Manifest
    -> best-effort remove the now-unreferenced immutable WAL

If a second active generation fills while the immutable one is still pending,
the foreground writer waits. This bounds the asynchronous write queue at one
immutable MemTable/WAL pair instead of admitting unbounded memory. A background
failure becomes sticky for later data operations and `Close`; a caller closes
and reopens after a visible-but-not-durable Manifest publication failure.
```

Every delete is stored as a tombstone rather than removing older bytes in place.
This is necessary because the older value may still exist in an immutable
SSTable. A future compaction stage can discard a tombstone only after it can prove
that no older visible value remains.

### Read and scan paths

`Get` checks the MemTable first because it contains newer sequence numbers, then
searches live SSTables from newest to oldest. A tombstone is returned internally
as the newest state but exposed to the caller as `NotFound`.

`Scan` creates borrowing iterators for the MemTable and each relevant SSTable,
then lazily merges them in byte-wise key order. Duplicate keys resolve to the
largest sequence and tombstones are removed from the public result. SSTable
iterators keep at most one decoded block at a time; a later block failure makes
the whole scan fail instead of returning a partial result. The public API still
materializes the final `vector<Entry>`.

### Asynchronous flush commit protocol

```text
active MemTable/WAL reaches threshold
    -> create and sync the replacement active WAL
    -> Manifest v3: old WAL becomes immutable, replacement is active
                                               <- rotation commit point
    -> background worker builds, validates, renames, and directory-syncs SST
    -> Manifest v3: add SSTable and clear immutable WAL
                                               <- flush commit point
    -> release immutable MemTable and best-effort remove its old WAL
```

### Compaction commit protocol

`Compact()` merges every published SSTable through the same internal iterator
path as `Scan`. It keeps the newest sequence for each key, drops tombstones, and
does not flush the MemTable or replace the active WAL.

```text
published SSTables
    -> merge all entries and build a replacement when a live value exists
    -> verify, rename, and directory-sync the replacement SSTable
    -> publish a Manifest containing zero or one table  <- commit point
    -> swap the in-memory reader set without allocation
    -> best-effort remove the old SSTables
```

A failure before Manifest publication leaves the old table set authoritative.
If the Manifest rename is visible but its directory sync fails, the current DB
handle rejects further data operations until it is closed and reopened.

### Orphan cleanup

Numbered files use canonical decimal names: numbers below one million are padded
to six digits, while larger numbers grow naturally. After complete recovery,
TinyLSM removes canonical `.wal`, `.sst`, `.sst.tmp`, and `MANIFEST.tmp` files
that are not referenced by the current Manifest. Noncanonical lookalikes and
unrelated files are always preserved. Listing, removal, or directory-sync
failures do not invalidate recovered data; later Open, flush, or compaction
attempts retry cleanup.

Before the Manifest rename, the old Manifest, WAL, and MemTable remain
authoritative even if the attempted flush created orphan files. A successful
Manifest rename makes the replacement visible; syncing the database directory
makes that replacement durable. If the directory sync fails, the current handle
enters a terminal error state because the replacement may be visible without a
durability guarantee, and the caller must close and reopen the database.

### Recovery path

```text
Open database
    -> load the authoritative Manifest
    -> validate and open every referenced SSTable
    -> read all live data blocks and verify true key/sequence metadata
    -> replay the optional immutable WAL, then the active WAL, into their
       corresponding MemTables
    -> require every replayed WAL sequence to increase beyond the published
       sequence and the previous replayed WAL
    -> truncate an incomplete WAL tail when recoverable
    -> continue from the highest recovered sequence number
    -> remove only canonical files not referenced by the recovered Manifest
```

Checksums detect accidental corruption in encoded WAL records, SSTable blocks,
and persisted metadata. Structural validation separately checks lengths, file
boundaries, ordering, indexes, and sequence metadata.

The WAL reader accepts the original version-1 single-operation records and the
version-2 batch record. A batch contains consecutive sequence numbers and one
checksum covering its complete payload; mixed old and new records can therefore
be recovered during an in-place format transition.

Manifest version 3 stores an active WAL, an optional immutable WAL, and live
tables in oldest-to-newest order, protected by CRC32C over its header and
Protobuf payload. The reader accepts fixed version-1 and version-2 snapshots,
which have no immutable WAL. Because the current SSTable format has no table-level
sequence properties block, startup reads every live data block to verify the
Manifest metadata; Open therefore costs `O(total live SSTable bytes)`.

## Current limitations

TinyLSM currently favors clarity and testability over feature breadth:

- Compaction is explicit, synchronous, and full-table only; there is no
  automatic trigger, background worker, or multi-level layout.
- A live SSTable reader retains its file handle until explicit compaction or
  close. Long write-only runs therefore need a MemTable threshold/workload that
  stays below the process file-descriptor limit; automated table-set management
  belongs to the next compaction goal.
- There is no Bloom filter. SSTable decoded blocks have an in-memory bounded
  LRU cache (8 MiB by default; `Options::block_cache_bytes = 0` disables it).
  The cache is not persistent and only stores successfully validated blocks.
- There is no group commit, snapshot isolation, or general multi-record
  transaction/rollback facility. Read operations can run concurrently, but a
  Scan holds a shared lock while materializing its result.
- The POSIX filesystem path is the implemented persistent backend.
- Packaging and installation rules are not implemented yet.

These constraints keep the durability boundary, recovery rules, and on-disk
formats visible while leaving clear next steps toward a fuller LSM engine.

## Performance baseline

The reproducible Release workloads, raw JSON output, macOS pre/post comparison,
same-machine Linux/LevelDB results, and fixed Lima VM definition are documented in
[`docs/performance/baseline-2026-09-06.md`](docs/performance/baseline-2026-09-06.md).
The follow-up work is split into independently verifiable Codex goals in
[`docs/plans/performance-extension-roadmap.md`](docs/plans/performance-extension-roadmap.md).
Goal 1's fixed-Linux profile, raw before/after JSON, cache accounting, and
concurrency result are in
[`docs/performance/read-path-2026-09-07.md`](docs/performance/read-path-2026-09-07.md).
Goal 2's bounded asynchronous-write design, two fixed-Linux raw JSON runs,
latency/stall/RSS evidence, and failure boundary are in
[`docs/performance/write-path-2026-09-07.md`](docs/performance/write-path-2026-09-07.md).
The baseline is descriptive and is not yet a CI performance gate.
The completed Linux run passed 97/97 tests in Debug, ASan/UBSan, and Clang TSan,
plus the Release build and deployment smoke test. Recreate it with
`tools/vm/tinylsm-linux.yaml`, then run `./scripts/run_linux_baseline.sh` inside
the VM to retain the environment, test logs, Release build log, complete
benchmark JSON, and verification evidence in one output directory.
