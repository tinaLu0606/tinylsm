#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace tinylsm::internal {

/// Keeps the oldest still-visible sequence.  Its lifetime is independent of a
/// DB handle so an outstanding Snapshot can safely outlive DB::Close().
class SnapshotState {
public:
  [[nodiscard]] bool TryRegister(std::uint64_t sequence, std::size_t maximum);
  void Unregister(std::uint64_t sequence) noexcept;
  [[nodiscard]] std::uint64_t OldestOr(std::uint64_t fallback) const noexcept;
  [[nodiscard]] std::size_t ActiveCount() const noexcept;
  void SetLastFullCompactionRetention(std::uint64_t versions,
                                      std::uint64_t bytes) noexcept;
  [[nodiscard]] std::uint64_t last_full_compaction_retained_versions() const noexcept;
  [[nodiscard]] std::uint64_t last_full_compaction_retained_bytes() const noexcept;

private:
  mutable std::mutex mutex_;
  std::map<std::uint64_t, std::size_t> active_;
  std::uint64_t last_full_compaction_retained_versions_ = 0;
  std::uint64_t last_full_compaction_retained_bytes_ = 0;
};

} // namespace tinylsm::internal
