# TinyLSM

TinyLSM is a compact C++20 key-value storage engine built to explore the core
mechanics of an LSM tree: write-ahead logging, ordered in-memory state, immutable
SSTables, manifest-based recovery, tombstones, checksums, and crash-aware file
publication.

The project is intentionally small enough to inspect end to end. It is a
learning-oriented prototype rather than a production database. The current
write path supports repeated synchronous flushes into an ordered set of
SSTables, range scans lazily merge those tables, and callers can explicitly run
synchronous full compaction. The V3 storage lifecycle is complete; concurrent
access is not implemented.

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

## Lab UI prototype

The experimental React and TypeScript frontend is maintained as the
`tools/lab_web` Git submodule. Initialize it and start the Vite development
server with:

```sh
git submodule update --init tools/lab_web
cd tools/lab_web
npm ci
npm run dev
```

The frontend provides Playground, Storage Explorer, Timeline, Workload,
Recovery, and Report workspaces through a typed `LabApi`. It keeps the
deterministic `MockLabApi` for offline UI work, and adds `HttpLabApi` for a
real local session. Start the C++ server on loopback, then run Vite with the
live transport selected:

```sh
./run build
./build/dev-debug/tools/tinylsm_lab_server --static-dir tools/lab_web/dist
cd tools/lab_web
VITE_LAB_API=live npm run dev
```

`tinylsm_lab_server` links TinyLSM directly; it never shells out to the CLI.
It accepts Open, Close, Reopen, Put, Get, Delete, Scan and Compact on one
serialized session, exposes copied internal diagnostic snapshots, lists real
directory files, retains the latest 10,000 operation/event records, and serves
SSE events with `Last-Event-ID` replay. It only listens on `127.0.0.1` (or
`::1` when embedded) and limits JSON request bodies to 8 MiB. `cpp-httplib`
v0.18.0 and `nlohmann/json` v3.11.3 are pinned through CMake FetchContent for
this local tool.

Pass `--static-dir tools/lab_web/dist` after `npm run build` to have the same
server also serve the packaged frontend; Vite remains useful for live frontend
development.

Live data is labelled `Live`; it is never filled from Mock fixtures. Paged
format decoding, resource sampling, deterministic workloads and destructive
recovery experiments remain unavailable until Goals 3 and 4. See
`tools/lab_web/README.md` for the frontend transport boundary and verification
steps.

## Developer commands

| Command | Purpose |
| --- | --- |
| `./run configure [preset]` | Generate build files without compiling |
| `./run build [preset]` | Configure and compile the selected preset |
| `./run test [preset]` | Configure, compile, and run CTest |
| `./run cli -- <args>` | Build and run `tinylsm_cli` |
| `./run format [--check]` | Apply or verify `clang-format` |
| `./run lint [preset]` | Run `clang-tidy` with the preset compilation database |
| `./run asan` | Build and test with AddressSanitizer and UBSan |
| `./run release` | Build the Release preset |
| `./run clean [preset]` | Clean the selected build tree |

Available CMake presets are:

- `dev-debug`: Debug build with tools and tests.
- `dev-asan-ubsan`: Debug build with AddressSanitizer and UndefinedBehaviorSanitizer.
- `release`: optimized build without the test targets.

The `package` command is reserved for future install/CPack support and currently
reports that packaging is not implemented.

## Public C++ API

The public interface is deliberately small: `Open`, `Put`, `Get`, `Delete`,
`Scan`, `Compact`, and `Close`. Expected storage failures are returned through
`Status` and `Result<T>` instead of exceptions.

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

Keys and values are byte strings. Database handles are movable but non-copyable
and are not thread-safe; callers must synchronize shared access externally.

## Implemented scope

- WAL-first `Put` and `Delete`, with optional per-write synchronization and
  repeated synchronous MemTable flushes.
- An ordered MemTable that keeps the newest sequence for each user key.
- Immutable, block-indexed SSTables with CRC32C integrity checks and complete
  validation of Manifest-referenced table data during startup.
- Tombstones that hide deleted values across memory and disk.
- Half-open ordered range scans that merge MemTable and SSTable state.
- Explicit synchronous full compaction into zero or one replacement SSTable.
- A versioned Manifest snapshot that records the authoritative WAL, ordered
  SSTable set, and sequence state while retaining version-1 read compatibility.
- Startup recovery through Manifest loading and active-WAL replay.
- Conservative orphan cleanup for canonical numbered WAL/SSTable names after
  successful recovery and at later maintenance checkpoints.
- Detection of malformed records, truncated data, checksum failures, invalid
  ordering, missing files, and size mismatches.
- Unit, integration, CLI, fault-injection, and recovery tests, plus ASan/UBSan
  test builds.

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
Put/Delete
    -> validate sizes and allocate a sequence number
    -> append InternalEntry to WAL
    -> optionally fsync WAL
    -> apply entry to MemTable
    -> flush when the configured memory threshold is reached
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

### Flush commit protocol

```text
MemTable
    -> build and verify temporary SSTable
    -> rename SSTable and sync the database directory
    -> create and sync a replacement WAL
    -> append the table to a new Manifest       <- durable commit point
    -> switch the live in-memory table/WAL state
    -> best-effort remove the old WAL
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
    -> replay its active WAL into a fresh MemTable
    -> require WAL sequences to increase beyond the published sequence
    -> truncate an incomplete WAL tail when recoverable
    -> continue from the highest recovered sequence number
    -> remove only canonical files not referenced by the recovered Manifest
```

Checksums detect accidental corruption in encoded WAL records, SSTable blocks,
and persisted metadata. Structural validation separately checks lengths, file
boundaries, ordering, indexes, and sequence metadata.

Manifest version 2 stores live tables in oldest-to-newest order and protects the
header plus Protobuf payload with CRC32C. The reader also accepts fixed version-1
zero-table and single-table snapshots. Because the current SSTable format has no
table-level sequence properties block, startup reads every live data block to
verify the Manifest metadata; Open therefore costs `O(total live SSTable bytes)`.

## Current limitations

TinyLSM currently favors clarity and testability over feature breadth:

- Compaction is explicit, synchronous, and full-table only; there is no
  automatic trigger, background worker, or multi-level layout.
- There is no Bloom filter or block cache.
- Flushes are synchronous; there is no immutable-MemTable/background worker.
- There is no transaction, write batch, snapshot, or concurrent writer support.
- The POSIX filesystem path is the implemented persistent backend.
- Packaging and installation rules are not implemented yet.

These constraints keep the durability boundary, recovery rules, and on-disk
formats visible while leaving clear next steps toward a fuller LSM engine.
