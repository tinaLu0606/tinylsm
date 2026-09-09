#pragma once

#include <cstddef>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iterator/internal_iterator.h"
#include "model/internal_entry.h"
#include "tinylsm/result.h"
#include "util/bytewise_less.h"

namespace tinylsm::internal {

/// An ordered, non-thread-safe collection containing all unflushed MVCC versions.
class MemTable {
public:
  /// Inserts one version. Per-key versions are stored in descending sequence
  /// order; tombstones remain stored so they can hide older disk values.
  Status Apply(InternalEntry entry);
  /// Applies a validated sequence atomically with respect to allocation
  /// failures: final values for touched keys are staged before entries_ changes.
  Status ApplyBatch(std::span<const InternalEntry> entries);
  [[nodiscard]] Result<InternalEntry>
  Get(std::string_view key,
      std::uint64_t sequence = std::numeric_limits<std::uint64_t>::max()) const;

  [[nodiscard]] Result<std::unique_ptr<InternalIterator>>
  NewIterator(std::string_view begin, std::string_view end) const;

  /// Materializes entries in byte-wise key order over [begin, end). An empty
  /// `end` means that the range is unbounded above.
  [[nodiscard]] std::vector<InternalEntry> Scan(std::string_view begin,
                                                std::string_view end) const;

  /// Returns approximate owned entry memory, not total container allocation.
  [[nodiscard]] std::size_t ApproximateMemoryUsage() const { return bytes_; }
  [[nodiscard]] bool Empty() const { return entries_.empty(); }
  void Clear();

private:
  class Iterator;
  std::map<std::string, std::vector<InternalEntry>, BytewiseLess> entries_;
  std::size_t bytes_ = 0;
};

} // namespace tinylsm::internal
