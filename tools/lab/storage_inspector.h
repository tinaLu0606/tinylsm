#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "tinylsm/result.h"
#include "tinylsm/status.h"

namespace tinylsm::lab {

struct InspectedEntry {
  std::string key;
  std::string value;
  std::uint64_t sequence = 0;
  bool tombstone = false;
};

struct WalRecordPageItem {
  std::uint64_t offset = 0;
  std::uint64_t encoded_bytes = 0;
  std::optional<InspectedEntry> entry;
  std::optional<Status> validation_error;
};

struct SstableBlockPageItem {
  std::size_t index = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::string smallest_key;
  std::string largest_key;
  std::vector<InspectedEntry> entries;
  std::optional<Status> validation_error;
};

template <typename T> struct PagedResult {
  std::vector<T> items;
  std::optional<std::uint64_t> next_cursor;
  bool has_more = false;
};

struct ManifestInspection {
  std::uint32_t format_version = 0;
  std::uint64_t active_wal = 0;
  std::uint64_t next_file_number = 0;
  std::uint64_t last_sequence = 0;
};

/// Read-only, bounded storage-format inspection for a canonical Lab DB path.
class StorageInspector {
public:
  static constexpr std::size_t kMaxPageItems = 100;
  static constexpr std::size_t kMaxRangeBytes = 256U * 1024U;

  explicit StorageInspector(std::filesystem::path database_path)
      : database_path_(std::move(database_path)) {}

  Result<ManifestInspection> Manifest() const;
  Result<PagedResult<WalRecordPageItem>>
  WalRecords(std::string_view name, std::uint64_t cursor, std::size_t limit) const;
  Result<PagedResult<SstableBlockPageItem>>
  SstableBlocks(std::string_view name, std::uint64_t cursor, std::size_t limit) const;
  Result<std::string> FileBytes(std::string_view name, std::uint64_t offset,
                                std::size_t length) const;

private:
  Result<std::filesystem::path> CanonicalFile(std::string_view name,
                                              bool allow_manifest) const;
  std::filesystem::path database_path_;
};

} // namespace tinylsm::lab
