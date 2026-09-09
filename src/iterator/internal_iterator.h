#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "model/internal_entry.h"
#include "tinylsm/result.h"

namespace tinylsm::internal {

/// Internal forward iterator shared by MemTable and SSTable read paths.
class InternalIterator {
public:
  virtual ~InternalIterator() = default;

  [[nodiscard]] virtual bool Valid() const noexcept = 0;
  [[nodiscard]] virtual const InternalEntry& entry() const = 0;
  /// Legacy test-only iterators may implement only forward traversal; they can
  /// accept the initial empty seek but reject repositioning.
  virtual Status Seek(std::string_view target) {
    return target.empty() ? Status::Ok()
                          : Status::NotSupported("internal iterator cannot seek");
  }
  virtual Status Next() = 0;
  [[nodiscard]] virtual const Status& status() const noexcept = 0;
};

/// Owns all inputs and exposes every version ordered by (user key, sequence
/// descending). Duplicate user-key/sequence pairs are corruption.
Result<std::unique_ptr<InternalIterator>>
NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> inputs);

/// Filters a version-ordered input into the value visible at `sequence` for
/// each user key, hiding tombstones. The resulting iterator remains pull-based.
Result<std::unique_ptr<InternalIterator>>
NewVisibilityIterator(std::unique_ptr<InternalIterator> input, std::uint64_t sequence);

/// Owns a captured MemTable vector and exposes it as an internal iterator.
Result<std::unique_ptr<InternalIterator>>
NewVectorIterator(std::vector<InternalEntry> entries, std::string_view begin,
                  std::string_view end);

} // namespace tinylsm::internal
