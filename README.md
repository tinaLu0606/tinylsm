# TinyLSM

TinyLSM is a compact C++20 key-value storage engine built to explore the core
mechanics of an LSM tree: write-ahead logging, ordered in-memory state, immutable
SSTables, manifest-based recovery, tombstones, checksums, and crash-aware file
publication.

The project is intentionally small enough to inspect end to end. It is a
learning-oriented prototype rather than a production database: the current
version supports one published SSTable and performs synchronous flushing, but it
does not yet implement compaction or concurrent access.

## Quick start

### Requirements

- CMake 3.24 or newer
- Ninja
- A C++20 compiler
- GoogleTest for test builds
- Protobuf (if it is not installed, CMake fetches the pinned v29.3 source)

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
`Scan`, and `Close`. Expected storage failures are returned through `Status` and
`Result<T>` instead of exceptions.

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

- WAL-first `Put` and `Delete`, with optional per-write synchronization.
- An ordered MemTable that keeps the newest sequence for each user key.
- Immutable, block-indexed SSTables with CRC32C integrity checks.
- Tombstones that hide deleted values across memory and disk.
- Half-open ordered range scans that merge MemTable and SSTable state.
- A Manifest that records the authoritative WAL, SSTable, and sequence state.
- Startup recovery through Manifest loading and active-WAL replay.
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
                                  |  SSTable  |
                                  | blocks +  |
                                  | index     |
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
- `tools/cli`: argument parsing and CLI command handlers.

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
falls back to the SSTable. A tombstone is returned internally as the newest state
but exposed to the caller as `NotFound`.

`Scan` reads the ordered memory and disk ranges, merges entries by byte-wise key
order, chooses the newest sequence for duplicate keys, and removes tombstones
from the public result.

### Flush commit protocol

```text
MemTable
    -> build and verify temporary SSTable
    -> rename SSTable and sync the database directory
    -> create and sync a replacement WAL
    -> replace and sync the new Manifest        <- durable commit point
    -> switch the live in-memory table/WAL state
    -> remove obsolete files when safe
```

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
    -> validate and open its referenced SSTable
    -> replay its active WAL into a fresh MemTable
    -> truncate an incomplete WAL tail when recoverable
    -> continue from the highest recovered sequence number
```

Checksums detect accidental corruption in encoded WAL records, SSTable blocks,
and persisted metadata. Structural validation separately checks lengths, file
boundaries, ordering, indexes, and sequence metadata.

## Current limitations

TinyLSM currently favors clarity and testability over feature breadth:

- Only one published SSTable is supported; a later write that would require a
  second flush returns `NotSupported`.
- There is no compaction, multi-level layout, Bloom filter, or block cache.
- Flushes are synchronous; there is no immutable-MemTable/background worker.
- There is no transaction, write batch, snapshot, or concurrent writer support.
- The POSIX filesystem path is the implemented persistent backend.
- Packaging and installation rules are not implemented yet.

These constraints keep the durability boundary, recovery rules, and on-disk
formats visible while leaving clear next steps toward a fuller LSM engine.
