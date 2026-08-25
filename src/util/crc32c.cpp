#include "util/crc32c.h"

namespace tinylsm::internal {
std::uint32_t Crc32c(std::span<const std::byte> data) {
  std::uint32_t crc = 0xffffffffU;
  for (const std::byte byte : data) {
    crc ^= std::to_integer<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return ~crc;
}
} // namespace tinylsm::internal
