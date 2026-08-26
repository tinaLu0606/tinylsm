#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "io/file.h"
#include "model/internal_entry.h"
#include "wal/log_format.h"

namespace tinylsm::internal {

/// Recovery metadata for the valid prefix consumed during replay.
struct WalReplayResult {
  std::uint64_t max_sequence = 0;
  std::uint64_t valid_bytes = 0;
  bool truncated_tail = false;
};

class WalReader {
public:
  WalReader(std::unique_ptr<SequentialFile> file, DecodeLimits limits)
      : file_(std::move(file)), limits_(limits) {}

  /// Replays complete records in file order through `apply`.
  ///
  /// An incomplete final header or payload is reported through truncated_tail
  /// and valid_bytes so the DB can truncate it. Invalid complete records and
  /// I/O errors fail replay without modifying the file.
  Result<WalReplayResult>
  Replay(const std::function<Status(const InternalEntry&)>& apply);

private:
  std::unique_ptr<SequentialFile> file_;
  DecodeLimits limits_;
};

} // namespace tinylsm::internal
