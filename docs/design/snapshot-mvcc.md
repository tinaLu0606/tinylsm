# Snapshot, Iterator and MVCC design

## Scope and contract

Goal 4 adds a process-local read-only Snapshot and a pull-based public Iterator.
`Snapshot` stores only a committed sequence number; it does not copy database
contents and does not survive Close/reopen. A Snapshot can be used only with
the DB that created it. `Get(key, snapshot)` and `NewIterator(..., snapshot)`
select the greatest entry sequence no greater than the Snapshot sequence.

```text
Put(a, old)  sequence 1
Snapshot()   sequence 1
Put(a, new)  sequence 2

latest Get(a)       -> new
Snapshot(1) Get(a)  -> old
```

`Iterator` captures copies of active/immutable MemTable versions and retains
shared ownership of selected SSTable readers. The DB state lock is released
before the caller receives it; `Next()` never owns that lock. Key/value views
are invalidated by the next `Seek()` or `Next()`.

## Internal ordering and visibility

All mutable and immutable sources use this strict ordering:

```text
(user_key ascending, sequence descending)

a@9, a@4, a@1, b@7, b@3
```

The merge layer emits every version and rejects duplicate `(user_key, sequence)`
pairs as corruption. The visibility layer skips entries newer than its read
sequence, selects the first remaining version for each user key, and suppresses
that key when the selected entry is a tombstone.

This ordering is compatible with existing SSTable v1 bytes: sequence and type
were already encoded per data entry. Older tables are a strict subset with only
one version per user key, so neither the SSTable footer nor Manifest requires a
format bump for this change.

## Lifetime and retention

`SnapshotState` tracks active sequence counts under its own mutex, independent
of DB handle lifetime. The default `Options::max_active_snapshots = 1024`
rejects excess Snapshot creation; zero explicitly disables that count bound.
`GetSnapshotMetrics()` reports active count, oldest active sequence, and the
entries/bytes emitted by the most recent full compaction.

For a full-table compaction with oldest active sequence `W`, retain all records
newer than `W` plus the first record at or below `W` for each user key. That
keeps every possible view whose sequence is at least `W`. With no Snapshot,
keep only the latest value; a latest tombstone and all older versions can be
discarded. A partial oldest-prefix compaction preserves every selected version,
because it does not rewrite the full version set.

Approximate long-Snapshot cost is:

```text
overwrite rate × average record bytes × oldest Snapshot age
```

The active Snapshot count is bounded by default, but retained bytes are not a
hard quota. Applications that keep a Snapshot for a long time should monitor
`GetSnapshotMetrics()` and release it promptly.

## Durability and recovery

Snapshots are never persisted. WAL replay and SSTable recovery preserve all
durably written MVCC entries, then select the current latest value for ordinary
reads. A process restart begins with no active Snapshot; a subsequent full
compaction may reclaim versions that were only retained for a prior process.

The existing durable commit points are unchanged:

```text
WAL append/sync -> MemTable apply
SST build/rename/SyncDir -> Manifest publication
compaction replacement rename/SyncDir -> Manifest publication
```

Only a durable Manifest publication permits old SSTables to become obsolete.
