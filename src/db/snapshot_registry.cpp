#include "db/snapshot_registry.h"

namespace tinylsm::internal {

bool SnapshotState::TryRegister(std::uint64_t sequence, std::size_t maximum) {
  std::scoped_lock lock(mutex_);
  std::size_t active = 0;
  for (const auto& item : active_)
    active += item.second;
  if (maximum != 0 && active >= maximum)
    return false;
  ++active_[sequence];
  return true;
}

void SnapshotState::Unregister(std::uint64_t sequence) noexcept {
  std::scoped_lock lock(mutex_);
  const auto it = active_.find(sequence);
  if (it == active_.end())
    return;
  if (--it->second == 0)
    active_.erase(it);
}

std::uint64_t SnapshotState::OldestOr(std::uint64_t fallback) const noexcept {
  std::scoped_lock lock(mutex_);
  return active_.empty() ? fallback : active_.begin()->first;
}

std::size_t SnapshotState::ActiveCount() const noexcept {
  std::scoped_lock lock(mutex_);
  std::size_t active = 0;
  for (const auto& item : active_)
    active += item.second;
  return active;
}

void SnapshotState::SetLastFullCompactionRetention(std::uint64_t versions,
                                                   std::uint64_t bytes) noexcept {
  std::scoped_lock lock(mutex_);
  last_full_compaction_retained_versions_ = versions;
  last_full_compaction_retained_bytes_ = bytes;
}

std::uint64_t SnapshotState::last_full_compaction_retained_versions() const noexcept {
  std::scoped_lock lock(mutex_);
  return last_full_compaction_retained_versions_;
}

std::uint64_t SnapshotState::last_full_compaction_retained_bytes() const noexcept {
  std::scoped_lock lock(mutex_);
  return last_full_compaction_retained_bytes_;
}

} // namespace tinylsm::internal
