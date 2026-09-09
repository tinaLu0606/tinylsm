#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace tinylsm {

class DB;

namespace internal {
class SnapshotState;
} // namespace internal

/// A process-local, read-only view of a DB at one committed sequence number.
///
/// A Snapshot does not copy database contents.  It remains valid after DB::Close(),
/// but it cannot be used with another DB handle or after destruction.  Snapshots
/// are deliberately non-copyable: keep the returned shared_ptr alive for as long
/// as the old view is needed.
class Snapshot final {
public:
  ~Snapshot();
  Snapshot(Snapshot&&) noexcept;
  Snapshot& operator=(Snapshot&&) noexcept;
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;

  [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }

private:
  friend class DB;
  Snapshot(std::shared_ptr<internal::SnapshotState> state, std::uint64_t sequence,
           std::size_t max_active);

  std::shared_ptr<internal::SnapshotState> state_;
  std::uint64_t sequence_ = 0;
  bool registered_ = false;
};

} // namespace tinylsm
