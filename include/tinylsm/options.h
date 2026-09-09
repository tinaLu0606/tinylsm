#pragma once

#include <cstddef>
#include <cstdint>

namespace tinylsm {

/// Configuration used when opening a persistent database.
struct Options {
  /// Approximate active MemTable size in bytes that triggers rotation to the
  /// one bounded immutable MemTable and a background flush.
  /// Must be non-zero.
  std::size_t memtable_bytes = 4U * 1024U * 1024U;

  /// Creates the database and initial files when they do not already exist.
  bool create_if_missing = true;

  /// Syncs the WAL before acknowledging each Put() or Delete().
  ///
  /// Disabling this can improve throughput, but recently acknowledged writes
  /// may be lost after a process crash, operating-system crash, or power loss.
  bool sync_on_write = true;

  /// Maximum encoded key size accepted by writes and WAL recovery, in bytes.
  std::uint32_t max_key_bytes = 4U * 1024U * 1024U;

  /// Maximum encoded value size accepted by writes and WAL recovery, in bytes.
  std::uint32_t max_value_bytes = 64U * 1024U * 1024U;

  /// Target uncompressed data-block size, in bytes. Must be non-zero.
  /// A single oversized entry is allowed to occupy a larger block by itself.
  std::size_t sstable_block_bytes = 16U * 1024U;

  /// Number of entries between complete restart keys in v2 SSTable data
  /// blocks. Must be non-zero. Smaller values favor random lookup; larger
  /// values favor prefix-compression density.
  std::uint32_t sstable_restart_interval = 16;

  /// Maximum logical charge of validated decoded SSTable blocks retained by
  /// this DB handle. Zero disables the Block Cache.
  std::size_t block_cache_bytes = 8U * 1024U * 1024U;

  /// Number of oldest SSTables in one simplified size-tiered compaction.
  /// Zero disables automatic background compaction; otherwise this must be at
  /// least two. The default bounds table count by scheduling a four-table job.
  std::size_t compaction_table_trigger = 4;

  /// Maximum process-local Snapshots held by one DB. Zero explicitly disables
  /// this bound. The default prevents forgotten Snapshot handles from making
  /// version retention unbounded by handle count.
  std::size_t max_active_snapshots = 1024;

  /// Maximum caller requests and bytes waiting for the group-commit leader.
  /// Writers wait for space rather than allocating an unbounded queue. Both
  /// values must be non-zero.
  std::size_t max_pending_write_requests = 64;
  std::size_t max_pending_write_bytes = 8U * 1024U * 1024U;

  /// Bounds one physical WAL group. A leader may combine complete public
  /// WriteBatch calls only while both limits hold. Both values must be non-zero.
  std::size_t max_group_commit_requests = 8;
  std::size_t max_group_commit_bytes = 1U * 1024U * 1024U;
};

} // namespace tinylsm
