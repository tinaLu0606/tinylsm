#pragma once

#include <cstddef>
#include <cstdint>

namespace tinylsm {

/// Configuration used when opening a persistent database.
struct Options {
  /// Approximate MemTable size in bytes that triggers a synchronous flush.
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
};

} // namespace tinylsm
