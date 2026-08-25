#pragma once

#include <cstdint>
#include <span>

namespace tinylsm::internal {
std::uint32_t Crc32c(std::span<const std::byte> data);
} // namespace tinylsm::internal
