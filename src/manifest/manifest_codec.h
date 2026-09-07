#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "tinylsm/result.h"

namespace tinylsm::internal {

inline constexpr std::size_t kManifestHeaderBytes = 16;
inline constexpr std::size_t kMaxManifestFileBytes = 128U * 1024U * 1024U;

struct TableMeta {
  std::uint64_t file_number = 0;
  std::uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence = 0;
  std::uint64_t max_sequence = 0;

  bool operator==(const TableMeta&) const = default;
};
struct ManifestSnapshot {
  std::uint64_t active_wal_number = 0;
  std::uint64_t next_file_number = 1;
  std::uint64_t last_sequence = 0;
  std::vector<TableMeta> live_tables;
  /// A sealed WAL paired with the one bounded immutable MemTable. Zero means
  /// that no background flush is pending. Kept last to preserve aggregate
  /// initialization used by version-1/version-2 format tests.
  std::uint64_t immutable_wal_number = 0;

  bool operator==(const ManifestSnapshot&) const = default;
};

/// Converts a complete snapshot to or from TinyLSM framing around a Protobuf
/// payload. Framing includes its own version, payload length, and CRC32C.
class ManifestCodec {
public:
  static Result<std::string> Encode(const ManifestSnapshot& snapshot);
  static Result<ManifestSnapshot> Decode(std::span<const std::byte> bytes);
};

} // namespace tinylsm::internal
