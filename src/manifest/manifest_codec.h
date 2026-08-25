#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "tinylsm/result.h"

namespace tinylsm::internal {

struct TableMeta {
  std::uint64_t file_number = 0;
  std::uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence = 0;
  std::uint64_t max_sequence = 0;
};
struct ManifestSnapshot {
  std::uint64_t active_wal_number = 0;
  std::uint64_t next_file_number = 1;
  std::uint64_t last_sequence = 0;
  std::optional<TableMeta> live_table;
};

class ManifestCodec {
public:
  static Result<std::string> Encode(const ManifestSnapshot& snapshot);
  static Result<ManifestSnapshot> Decode(std::span<const std::byte> bytes);
};

} // namespace tinylsm::internal
