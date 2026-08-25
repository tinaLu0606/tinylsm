#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "io/file.h"
#include "model/internal_entry.h"
#include "sstable/sstable_format.h"

namespace tinylsm::internal {

struct BuiltTableInfo {
  std::uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence = 0;
  std::uint64_t max_sequence = 0;
};

class SSTableBuilder {
public:
  SSTableBuilder(std::unique_ptr<WritableFile> file, std::size_t block_bytes)
      : file_(std::move(file)), block_bytes_(block_bytes) {}
  Status Add(InternalEntry entry);
  Result<BuiltTableInfo> Finish();

private:
  Status FlushBlock();
  std::unique_ptr<WritableFile> file_;
  std::size_t block_bytes_;
  std::vector<InternalEntry> pending_;
  std::size_t pending_bytes_ = 0;
  std::vector<BlockMeta> blocks_;
  std::uint64_t offset_ = 0;
  BuiltTableInfo info_;
  bool has_entries_ = false;
  bool finished_ = false;
};

} // namespace tinylsm::internal
