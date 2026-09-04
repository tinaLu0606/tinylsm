#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "io/file.h"
#include "iterator/internal_iterator.h"
#include "model/internal_entry.h"
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
  Open(std::unique_ptr<RandomAccessFile> file);
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
                std::vector<BlockMeta> blocks)
      : file_(std::move(file)), file_size_(file_size), blocks_(std::move(blocks)) {}
  Result<std::vector<InternalEntry>> ReadBlock(const BlockMeta& meta) const;
  std::unique_ptr<RandomAccessFile> file_;
  std::uint64_t file_size_ = 0;
  std::vector<BlockMeta> blocks_;
};

} // namespace tinylsm::internal
