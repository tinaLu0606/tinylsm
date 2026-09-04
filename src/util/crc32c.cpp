#include "util/crc32c.h"

namespace tinylsm::internal {
namespace {
std::uint32_t ExtendCrc32c(std::uint32_t crc, std::span<const std::byte> data) {
  for (const std::byte byte : data) {
    crc ^= std::to_integer<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return crc;
}
} // namespace

std::uint32_t Crc32c(std::span<const std::byte> data) {
  const auto crc = ExtendCrc32c(0xffffffffU, data);
  return ~crc;
}

std::uint32_t Crc32c(std::span<const std::byte> first,
                     std::span<const std::byte> second) {
  const auto crc = ExtendCrc32c(ExtendCrc32c(0xffffffffU, first), second);
  return ~crc;
}
} // namespace tinylsm::internal
