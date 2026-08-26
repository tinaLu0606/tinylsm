#pragma once

#include <memory>

#include "io/file.h"
#include "model/internal_entry.h"
#include "wal/log_format.h"

namespace tinylsm::internal {

class WalWriter {
public:
  WalWriter(std::unique_ptr<WritableFile> file, DecodeLimits limits)
      : file_(std::move(file)), limits_(limits) {}

  /// Encodes and appends one complete record; durability requires Sync().
  Status Append(const InternalEntry& entry);
  Status Sync() { return file_->Sync(); }
  Status Close() { return file_->Close(); }

private:
  std::unique_ptr<WritableFile> file_;
  DecodeLimits limits_;
};

} // namespace tinylsm::internal
