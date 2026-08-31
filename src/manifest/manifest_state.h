#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

#include "io/file.h"
#include "manifest/manifest_codec.h"

namespace tinylsm::internal {

enum class ManifestPublishState : std::uint8_t {
  kNotPublished,
  kVisibleNotDurable,
  kDurable,
};

/// Reports both an error and how far Manifest publication progressed.
class [[nodiscard]] ManifestPublishOutcome {
public:
  static ManifestPublishOutcome NotPublished(Status status);
  static ManifestPublishOutcome VisibleNotDurable(Status status);
  static ManifestPublishOutcome Durable();

  [[nodiscard]] ManifestPublishState state() const { return state_; }
  [[nodiscard]] const Status& status() const { return status_; }
  [[nodiscard]] bool durable() const {
    return state_ == ManifestPublishState::kDurable;
  }

private:
  ManifestPublishOutcome(ManifestPublishState state, Status status)
      : state_(state), status_(std::move(status)) {}

  ManifestPublishState state_;
  Status status_;
};

/// Owns the last committed Manifest snapshot for a database directory.
class ManifestState {
public:
  ManifestState(FileSystem& fs, std::filesystem::path db_path, ManifestSnapshot current)
      : fs_(fs), db_path_(std::move(db_path)), current_(std::move(current)) {}
  static Result<ManifestSnapshot> Load(FileSystem& fs,
                                       const std::filesystem::path& db_path);

  /// Publishes `next` through a synced temporary file, rename, and directory
  /// sync. The outcome distinguishes a pre-rename failure from an uncertain
  /// directory-sync failure. current() changes only for a durable publication.
  ManifestPublishOutcome Publish(const ManifestSnapshot& next);
  [[nodiscard]] const ManifestSnapshot& current() const { return current_; }

private:
  FileSystem& fs_;
  std::filesystem::path db_path_;
  ManifestSnapshot current_;
};

} // namespace tinylsm::internal
