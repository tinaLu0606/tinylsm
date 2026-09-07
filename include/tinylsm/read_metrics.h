#pragma once

#include <cstddef>
#include <cstdint>

namespace tinylsm {

/// A point-in-time copy of cumulative read-path counters for one DB handle.
struct ReadMetrics {
  std::uint64_t table_probes = 0;
  std::uint64_t block_reads = 0;
  std::uint64_t block_decodes = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t cache_inserts = 0;
  std::uint64_t cache_evictions = 0;
  std::size_t cache_charge_bytes = 0;
  std::size_t cache_capacity_bytes = 0;
  std::uint64_t read_lock_acquisitions = 0;
  std::uint64_t read_lock_wait_nanoseconds = 0;
  std::uint64_t write_lock_acquisitions = 0;
  std::uint64_t write_lock_wait_nanoseconds = 0;
};

} // namespace tinylsm
