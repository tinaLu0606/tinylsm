#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "io/file.h"
#include "iterator/internal_iterator.h"
#include "model/internal_entry.h"
#include "sstable/block_cache.h"
#include "sstable/sstable_format.h"

namespace tinylsm::internal {

struct SSTableProperties {
  std::uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence = 0;
  std::uint64_t max_sequence = 0;
};

/// Reads an immutable SSTable using its in-memory index and on-demand blocks.
class SSTableReader {
public:
  /// Validates and loads the footer and index without loading all data blocks.
  static Result<std::unique_ptr<SSTableReader>>
  Open(std::unique_ptr<RandomAccessFile> file, std::uint64_t table_number = 0,
       std::shared_ptr<BlockCache> block_cache = {},
       std::shared_ptr<ReadMetricsState> metrics = {});
  [[nodiscard]] std::uint64_t file_size() const { return file_size_; }
  Result<SSTableProperties> ValidateAndGetProperties() const;
  Result<InternalEntry> Get(std::string_view key) const;

  Result<std::unique_ptr<InternalIterator>> NewIterator(std::string_view begin,
                                                        std::string_view end) const;

  /// Materializes entries in [begin, end); an empty `end` is unbounded above.
  Result<std::vector<InternalEntry>> Scan(std::string_view begin,
                                          std::string_view end) const;

private:
  class Iterator;
  SSTableReader(std::unique_ptr<RandomAccessFile> file, std::uint64_t file_size,
                std::vector<BlockMeta> blocks, std::uint64_t table_number,
                std::shared_ptr<BlockCache> block_cache,
                std::shared_ptr<ReadMetricsState> metrics)
      : file_(std::move(file)), file_size_(file_size), blocks_(std::move(blocks)),
        table_number_(table_number), block_cache_(std::move(block_cache)),
        metrics_(std::move(metrics)) {}
  Result<BlockCache::BlockPtr> ReadBlock(const BlockMeta& meta,
                                         bool use_cache = true) const;
  std::unique_ptr<RandomAccessFile> file_;
  std::uint64_t file_size_ = 0;
  std::vector<BlockMeta> blocks_;
  std::uint64_t table_number_ = 0;
  std::shared_ptr<BlockCache> block_cache_;
  std::shared_ptr<ReadMetricsState> metrics_;
};

} // namespace tinylsm::internal
