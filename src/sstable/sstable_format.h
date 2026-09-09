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
inline constexpr std::uint32_t kSstableVersionV1 = 1;
inline constexpr std::uint32_t kSstableVersion = 2;
inline constexpr std::size_t kSstableV1FooterSize = 36;
inline constexpr std::size_t kSstableFooterSize = 52;
inline constexpr std::uint32_t kDefaultRestartInterval = 16;

struct BlockMeta {
  std::string first_key;
  std::string last_key;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};
struct Footer {
  std::uint32_t version = kSstableVersion;
  std::uint64_t index_offset = 0;
  std::uint64_t index_size = 0;
  std::uint64_t properties_offset = 0;
  std::uint64_t properties_size = 0;
};
struct TableProperties {
  std::uint64_t entry_count = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence = 0;
  std::uint64_t max_sequence = 0;
};

/// Data blocks, the index, and the fixed-size footer each carry an independent
/// CRC32C check. Decode functions reject malformed lengths and key ordering.
/// v2 stores prefix-compressed user keys and restart offsets. The v1 helpers
/// remain only for compatibility tests and Reader support.
Result<std::string>
EncodeDataBlock(const std::vector<InternalEntry>& entries,
                std::uint32_t restart_interval = kDefaultRestartInterval);
Result<std::string> EncodeDataBlockV1(const std::vector<InternalEntry>& entries);
Result<std::vector<InternalEntry>>
DecodeDataBlock(std::span<const std::byte> bytes,
                std::uint32_t version = kSstableVersion);
/// Finds one visible entry. For v2 this reconstructs only the selected restart
/// group after verifying the whole-block checksum.
Result<InternalEntry> FindDataBlockEntry(std::span<const std::byte> bytes,
                                         std::uint32_t version, std::string_view key,
                                         std::uint64_t sequence);
Result<std::string> EncodeIndex(const std::vector<BlockMeta>& blocks);
Result<std::vector<BlockMeta>> DecodeIndex(std::span<const std::byte> bytes);
Result<std::string> EncodeProperties(const TableProperties& properties);
Result<TableProperties> DecodeProperties(std::span<const std::byte> bytes);
std::string EncodeFooter(const Footer& footer);
std::string EncodeFooterV1(std::uint64_t index_offset, std::uint64_t index_size);
Result<Footer> DecodeFooter(std::span<const std::byte> bytes);

} // namespace tinylsm::internal
