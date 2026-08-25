#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "io/file.h"
#include "model/internal_entry.h"
#include "sstable/sstable_format.h"

namespace tinylsm::internal {

class SSTableReader {
public:
  static Result<std::unique_ptr<SSTableReader>> Open(std::unique_ptr<RandomAccessFile> file);
  Result<InternalEntry> Get(std::string_view key) const;
  Result<std::vector<InternalEntry>> Scan(std::string_view begin, std::string_view end) const;

private:
  SSTableReader(std::unique_ptr<RandomAccessFile> file, std::vector<BlockMeta> blocks)
      : file_(std::move(file)), blocks_(std::move(blocks)) {}
  Result<std::vector<InternalEntry>> ReadBlock(const BlockMeta& meta) const;
  std::unique_ptr<RandomAccessFile> file_;
  std::vector<BlockMeta> blocks_;
};

} // namespace tinylsm::internal
