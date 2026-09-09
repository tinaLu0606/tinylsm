#pragma once

#include <cstdint>

namespace tinylsm {

/// Gauges for the process-local Snapshot retention boundary.
struct SnapshotMetrics {
  std::uint64_t active_snapshots = 0;
  /// Zero means no Snapshot is currently active.
  std::uint64_t oldest_snapshot_sequence = 0;
  /// Entries and logical bytes retained by the most recent full-table
  /// compaction. They make the cost of a long-lived Snapshot observable.
  std::uint64_t last_full_compaction_retained_versions = 0;
  std::uint64_t last_full_compaction_retained_bytes = 0;
};

} // namespace tinylsm
