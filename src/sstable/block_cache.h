#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "model/internal_entry.h"
#include "tinylsm/read_metrics.h"

namespace tinylsm::internal {

class ReadMetricsState {
public:
  [[nodiscard]] ReadMetrics Snapshot() const noexcept;

  std::atomic<std::uint64_t> point_lookups{0};
  std::atomic<std::uint64_t> range_scans{0};
  std::atomic<std::uint64_t> scan_table_inputs{0};
  std::atomic<std::uint64_t> table_probes{0};
  std::atomic<std::uint64_t> block_reads{0};
  std::atomic<std::uint64_t> block_decodes{0};
  std::atomic<std::uint64_t> cache_hits{0};
  std::atomic<std::uint64_t> cache_misses{0};
  std::atomic<std::uint64_t> cache_inserts{0};
  std::atomic<std::uint64_t> cache_evictions{0};
  std::atomic<std::size_t> cache_charge_bytes{0};
  std::atomic<std::size_t> cache_capacity_bytes{0};
  std::atomic<std::uint64_t> read_lock_acquisitions{0};
  std::atomic<std::uint64_t> read_lock_wait_nanoseconds{0};
  std::atomic<std::uint64_t> write_lock_acquisitions{0};
  std::atomic<std::uint64_t> write_lock_wait_nanoseconds{0};
};

struct BlockCacheKey {
  std::uint64_t table_number = 0;
  std::uint64_t block_offset = 0;

  bool operator==(const BlockCacheKey&) const = default;
};

class BlockCache {
public:
  using Block = std::vector<InternalEntry>;
  using BlockPtr = std::shared_ptr<const Block>;

  BlockCache(std::size_t capacity_bytes,
             std::shared_ptr<ReadMetricsState> metrics) noexcept;

  [[nodiscard]] bool enabled() const noexcept { return capacity_bytes_ != 0; }
  [[nodiscard]] BlockPtr Lookup(BlockCacheKey key);
  void Insert(BlockCacheKey key, BlockPtr block, std::size_t charge);
  void EraseTable(std::uint64_t table_number);

private:
  struct KeyHash {
    std::size_t operator()(BlockCacheKey key) const noexcept;
  };
  struct Entry {
    BlockCacheKey key;
    BlockPtr block;
    std::size_t charge = 0;
  };

  using Lru = std::list<Entry>;
  using Index = std::unordered_map<BlockCacheKey, Lru::iterator, KeyHash>;

  void Remove(Lru::iterator entry);

  const std::size_t capacity_bytes_;
  std::shared_ptr<ReadMetricsState> metrics_;
  std::mutex mutex_;
  std::size_t charge_bytes_ = 0;
  Lru lru_;
  Index index_;
};

std::size_t BlockCharge(const BlockCache::Block& block) noexcept;

} // namespace tinylsm::internal
