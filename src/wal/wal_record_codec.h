#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "model/internal_entry.h"
#include "tinylsm/result.h"

namespace tinylsm::internal {

inline constexpr std::uint32_t kWalMagic = 0x314c4157U; // WAL1
inline constexpr std::uint16_t kWalVersion = 1;
inline constexpr std::uint16_t kWalBatchVersion = 2;
inline constexpr std::uint8_t kWalBatchRecordType = 3;
inline constexpr std::size_t kWalHeaderSize = 16;
inline constexpr std::size_t kWalPayloadHeaderSize = 16;
inline constexpr std::size_t kWalBatchPayloadHeaderSize = 4;
inline constexpr std::size_t kWalBatchEntryHeaderSize = 20;
inline constexpr std::uint64_t kMaxWalBatchBytes = 128U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxWalBatchOperations = 1'000'000;

/// Allocation limits applied before WAL field lengths are trusted.
struct DecodeLimits {
  std::uint32_t max_key_bytes = 4U * 1024U * 1024U;
  std::uint32_t max_value_bytes = 64U * 1024U * 1024U;
};

/// Encodes or decodes exactly one complete little-endian WAL record.
Result<std::string> EncodeWalRecord(const InternalEntry& entry,
                                    const DecodeLimits& limits);
Result<InternalEntry> DecodeWalRecord(std::span<const std::byte> record,
                                      const DecodeLimits& limits);

/// Encodes one non-empty batch as a version-2 WAL record. Every entry must
/// have a non-zero sequence and batch sequences must be consecutive.
Result<std::string> EncodeWalBatch(std::span<const InternalEntry> entries,
                                   const DecodeLimits& limits);
Result<std::vector<InternalEntry>> DecodeWalBatch(std::span<const std::byte> record,
                                                  const DecodeLimits& limits);

} // namespace tinylsm::internal
