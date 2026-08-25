#pragma once

#include <cstddef>
#include <cstdint>

namespace tinylsm {

struct Options {
  std::size_t memtable_bytes = 4U * 1024U * 1024U;
  bool create_if_missing = true;
  bool sync_on_write = true;
  std::uint32_t max_key_bytes = 4U * 1024U * 1024U;
  std::uint32_t max_value_bytes = 64U * 1024U * 1024U;
  std::size_t sstable_block_bytes = 16U * 1024U;
};

} // namespace tinylsm
