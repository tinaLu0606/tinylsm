#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tinylsm/options.h"
#include "tinylsm/result.h"
#include "tinylsm/status.h"

namespace tinylsm {
class DB;

namespace internal {

/// A read-only copy of one table's committed Manifest metadata. This is an
/// internal diagnostic type; it is deliberately not part of the public DB API.
struct DiagnosticTableSnapshot {
  std::uint64_t file_number = 0;
  std::uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence = 0;
  std::uint64_t max_sequence = 0;
};

/// Copies mutable DB state for observers without exposing DB-owned references.
struct DiagnosticSnapshot {
  Options options;
  std::size_t memtable_bytes = 0;
  std::size_t memtable_entries = 0;
  std::uint64_t next_sequence = 1;
  std::uint64_t active_wal_number = 0;
  std::uint64_t last_sequence = 0;
  std::vector<DiagnosticTableSnapshot> live_tables;
  std::size_t pending_cleanup = 0;
  bool closed = false;
  std::optional<Status> terminal_error;
};

/// Internal bridge used by the Lab server. Snapshot() takes the same internal
/// mutex as public DB operations and returns an independent copy.
class DBLabPeer {
public:
  static Result<DiagnosticSnapshot> Snapshot(const DB& db);
};

} // namespace internal
} // namespace tinylsm
