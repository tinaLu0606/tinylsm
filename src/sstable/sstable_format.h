#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "model/internal_entry.h"
#include "tinylsm/result.h"

namespace tinylsm::internal {

inline constexpr std::uint64_t kSstableMagic = 0x315453534d534c54ULL;
inline constexpr std::uint32_t kSstableVersion = 1;
inline constexpr std::size_t kSstableFooterSize = 36;

struct BlockMeta {
  std::string first_key;
  std::string last_key;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};
struct Footer {
  std::uint64_t index_offset = 0;
  std::uint64_t index_size = 0;
};

/// Data blocks, the index, and the fixed-size footer each carry an independent
/// CRC32C check. Decode functions reject malformed lengths and key ordering.
Result<std::string> EncodeDataBlock(const std::vector<InternalEntry>& entries);
Result<std::vector<InternalEntry>> DecodeDataBlock(std::span<const std::byte> bytes);
Result<std::string> EncodeIndex(const std::vector<BlockMeta>& blocks);
Result<std::vector<BlockMeta>> DecodeIndex(std::span<const std::byte> bytes);
std::string EncodeFooter(const Footer& footer);
Result<Footer> DecodeFooter(std::span<const std::byte> bytes);

} // namespace tinylsm::internal
