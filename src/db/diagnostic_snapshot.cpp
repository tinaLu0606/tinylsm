#include "db/diagnostic_snapshot.h"

#include "db/db_impl.h"
#include "tinylsm/db.h"

namespace tinylsm::internal {

Result<DiagnosticSnapshot> DBLabPeer::Snapshot(const DB& db) {
  if (!db.impl_)
    return Status::AlreadyClosed("database implementation is unavailable");

  const auto& impl = *db.impl_;
  if (!impl.manifest_)
    return Status::Corruption("database has no Manifest state");

  DiagnosticSnapshot out;
  out.options = impl.options_;
  out.memtable_bytes = impl.memtable_.ApproximateMemoryUsage();
  out.memtable_entries = impl.memtable_.Scan({}, {}).size();
  out.next_sequence = impl.next_sequence_;
  out.active_wal_number = impl.manifest_->current().active_wal_number;
  out.last_sequence = impl.manifest_->current().last_sequence;
  out.pending_cleanup = impl.pending_cleanup_.size();
  out.closed = impl.closed_;
  out.terminal_error = impl.terminal_error_;
  out.live_tables.reserve(impl.manifest_->current().live_tables.size());
  for (const auto& table : impl.manifest_->current().live_tables) {
    out.live_tables.push_back({table.file_number, table.file_size, table.smallest_key,
                               table.largest_key, table.min_sequence,
                               table.max_sequence});
  }
  return out;
}

} // namespace tinylsm::internal
