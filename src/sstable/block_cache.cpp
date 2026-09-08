#include "sstable/block_cache.h"

#include <functional>
#include <limits>

namespace tinylsm::internal {
namespace {
template <typename T> T Load(const std::atomic<T>& value) noexcept {
  return value.load(std::memory_order_relaxed);
}

void AddSaturating(std::size_t& total, std::size_t value) noexcept {
  if (value > std::numeric_limits<std::size_t>::max() - total) {
    total = std::numeric_limits<std::size_t>::max();
    return;
  }
  total += value;
}
} // namespace

ReadMetrics ReadMetricsState::Snapshot() const noexcept {
  return {Load(point_lookups),
          Load(range_scans),
          Load(scan_table_inputs),
          Load(table_probes),
          Load(block_reads),
          Load(block_decodes),
          Load(cache_hits),
          Load(cache_misses),
          Load(cache_inserts),
          Load(cache_evictions),
          Load(cache_charge_bytes),
          Load(cache_capacity_bytes),
          Load(read_lock_acquisitions),
          Load(read_lock_wait_nanoseconds),
          Load(write_lock_acquisitions),
          Load(write_lock_wait_nanoseconds)};
}

BlockCache::BlockCache(std::size_t capacity_bytes,
                       std::shared_ptr<ReadMetricsState> metrics) noexcept
    : capacity_bytes_(capacity_bytes), metrics_(std::move(metrics)) {
  metrics_->cache_capacity_bytes.store(capacity_bytes_, std::memory_order_relaxed);
}

std::size_t BlockCache::KeyHash::operator()(BlockCacheKey key) const noexcept {
  const auto first = std::hash<std::uint64_t>{}(key.table_number);
  const auto second = std::hash<std::uint64_t>{}(key.block_offset);
  return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
}

BlockCache::BlockPtr BlockCache::Lookup(BlockCacheKey key) {
  if (capacity_bytes_ == 0)
    return {};

  std::scoped_lock lock(mutex_);
  const auto found = index_.find(key);
  if (found == index_.end()) {
    metrics_->cache_misses.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  lru_.splice(lru_.begin(), lru_, found->second);
  metrics_->cache_hits.fetch_add(1, std::memory_order_relaxed);
  return found->second->block;
}

void BlockCache::Insert(BlockCacheKey key, BlockPtr block, std::size_t charge) {
  if (capacity_bytes_ == 0 || !block || charge > capacity_bytes_)
    return;

  std::scoped_lock lock(mutex_);
  const auto found = index_.find(key);
  if (found != index_.end()) {
    lru_.splice(lru_.begin(), lru_, found->second);
    return;
  }
  while (!lru_.empty() && charge > capacity_bytes_ - charge_bytes_) {
    Remove(std::prev(lru_.end()));
    metrics_->cache_evictions.fetch_add(1, std::memory_order_relaxed);
  }

  lru_.push_front({key, std::move(block), charge});
  try {
    index_.emplace(key, lru_.begin());
  } catch (...) {
    lru_.pop_front();
    throw;
  }
  charge_bytes_ += charge;
  metrics_->cache_charge_bytes.store(charge_bytes_, std::memory_order_relaxed);
  metrics_->cache_inserts.fetch_add(1, std::memory_order_relaxed);
}

void BlockCache::EraseTable(std::uint64_t table_number) {
  if (capacity_bytes_ == 0)
    return;

  std::scoped_lock lock(mutex_);
  for (auto entry = lru_.begin(); entry != lru_.end();) {
    if (entry->key.table_number != table_number) {
      ++entry;
      continue;
    }
    const auto remove = entry++;
    Remove(remove);
  }
  metrics_->cache_charge_bytes.store(charge_bytes_, std::memory_order_relaxed);
}

void BlockCache::Remove(Lru::iterator entry) {
  charge_bytes_ -= entry->charge;
  index_.erase(entry->key);
  lru_.erase(entry);
}

std::size_t BlockCharge(const BlockCache::Block& block) noexcept {
  std::size_t charge = sizeof(BlockCache::Block) + sizeof(BlockCache::BlockPtr) +
                       sizeof(BlockCacheKey) + 4 * sizeof(void*);
  AddSaturating(charge, block.capacity() * sizeof(InternalEntry));
  for (const auto& entry : block) {
    AddSaturating(charge, entry.user_key.capacity());
    AddSaturating(charge, entry.value.capacity());
  }
  return charge;
}

} // namespace tinylsm::internal
