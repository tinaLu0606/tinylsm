#pragma once

#include <cstdint>
#include <span>

namespace tinylsm::internal {
std::uint32_t Crc32c(std::span<const std::byte> data);
std::uint32_t Crc32c(std::span<const std::byte> first,
                     std::span<const std::byte> second);
} // namespace tinylsm::internal
