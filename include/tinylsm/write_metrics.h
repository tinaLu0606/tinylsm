#pragma once

#include <cstddef>
#include <cstdint>

namespace tinylsm {

/// A point-in-time copy of cumulative write-path counters for one DB handle.
struct WriteMetrics {
  std::uint64_t writes = 0;
  std::uint64_t write_batches = 0;
  std::uint64_t wal_syncs = 0;
  std::uint64_t memtable_rotations = 0;
  std::uint64_t background_flushes = 0;
  std::uint64_t background_flush_failures = 0;
  std::uint64_t backpressure_waits = 0;
  std::uint64_t backpressure_wait_nanoseconds = 0;
  std::size_t background_queue_depth = 0;
  std::size_t max_background_queue_depth = 0;
  std::size_t immutable_memtable_bytes = 0;
  std::size_t max_immutable_memtable_bytes = 0;
};

} // namespace tinylsm
