#pragma once

#include <cstddef>
#include <cstdint>

namespace tinylsm {

/// A point-in-time copy of cumulative compaction counters and current table
/// state for one DB handle. The byte counters are physical SSTable bytes, so
/// callers can derive write and space amplification for a known workload.
struct CompactionMetrics {
  std::uint64_t compactions = 0;
  std::uint64_t background_compactions = 0;
  std::uint64_t compaction_failures = 0;
  std::uint64_t compaction_input_tables = 0;
  std::uint64_t compaction_output_tables = 0;
  std::uint64_t compaction_input_bytes = 0;
  std::uint64_t compaction_output_bytes = 0;
  std::uint64_t flush_output_bytes = 0;
  std::size_t table_count = 0;
  std::size_t live_sstable_bytes = 0;
  std::size_t compaction_debt_tables = 0;
  std::size_t compaction_debt_bytes = 0;
};

} // namespace tinylsm
