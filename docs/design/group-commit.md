# Bounded group commit

## Scope

Goal 6 replaces the former per-call write serialization with a bounded queue.
`Put`, `Delete`, and `Write(WriteBatch)` each submit one complete public
request. One leader removes a bounded prefix of queued requests, assigns one
continuous sequence range, writes it as one WAL batch, optionally syncs once,
then applies it to the active MemTable.

```text
callers -> bounded request queue -> leader
                                  -> WAL AppendBatch(all group entries)
                                  -> optional WAL Sync once
                                  -> MemTable ApplyBatch
                                  -> wake every request in the group
```

`Options::max_pending_write_requests` and
`Options::max_pending_write_bytes` bound queued memory. Writers wait for room;
a single request larger than the byte bound is rejected. A leader also obeys
`max_group_commit_requests` and `max_group_commit_bytes`, so an oversized but
valid request runs alone rather than making the group unbounded.

## Atomicity, ordering, and recovery

The leader never splits a caller's `WriteBatch`. Operations keep FIFO request
order, then their original in-batch order, and therefore receive consecutive
sequences. All group entries are encoded in one existing WAL batch frame.
Consequently a truncated final frame is discarded as a whole during recovery;
it cannot expose half of a public batch.

Grouping is a physical WAL optimization, not a new public transaction API:
callers still receive individual Status values and have no cross-request
rollback or isolation guarantee. A WAL append or sync error is reported to
every request in that physical group. This preserves TinyLSM's existing
uncertain-write contract: callers must not infer that a failed write was absent
from durable storage, and reopen determines recovered state.

If a standard-library or dependency exception escapes the leader's write path,
the leader rethrows that original exception to preserve the public API contract.
Every request already selected for the group, and any queued follower, is first
woken with an `IOError`; no follower is left waiting on a departed leader.

## Close and metrics

`Close()` first stops queue admission and waits for the active leader to drain
already admitted requests. It then follows the existing WAL/background-flush
close protocol. A close-WAL-sync failure reopens queue admission because the
handle remains usable for retry.

`GetWriteMetrics()` adds physical group count, grouped request count, cumulative
queue wait time, and current/maximum queue depth. They are observability
counters, not a throughput or latency conclusion. Formal multiwriter
benchmarking remains separately scheduled.
