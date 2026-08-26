#pragma once

#include <filesystem>
#include <optional>

#include "io/file.h"
#include "manifest/manifest_codec.h"

namespace tinylsm::internal {

/// Owns the last committed Manifest snapshot for a database directory.
class ManifestState {
public:
  ManifestState(FileSystem& fs, std::filesystem::path db_path, ManifestSnapshot current)
      : fs_(fs), db_path_(std::move(db_path)), current_(std::move(current)) {}
  static Result<ManifestSnapshot> Load(FileSystem& fs,
                                       const std::filesystem::path& db_path);

  /// Publishes `next` through a synced temporary file, rename, and directory
  /// sync. current() changes only after all publication steps succeed.
  Status Publish(const ManifestSnapshot& next);
  [[nodiscard]] const ManifestSnapshot& current() const { return current_; }

private:
  FileSystem& fs_;
  std::filesystem::path db_path_;
  ManifestSnapshot current_;
};

} // namespace tinylsm::internal
