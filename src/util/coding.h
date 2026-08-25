#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tinylsm::internal {

void PutFixed16(std::string& output, std::uint16_t value);
void PutFixed32(std::string& output, std::uint32_t value);
void PutFixed64(std::string& output, std::uint64_t value);
bool GetFixed16(std::span<const std::byte> input, std::size_t offset, std::uint16_t& value);
bool GetFixed32(std::span<const std::byte> input, std::size_t offset, std::uint32_t& value);
bool GetFixed64(std::span<const std::byte> input, std::size_t offset, std::uint64_t& value);
std::span<const std::byte> AsBytes(std::string_view input);

} // namespace tinylsm::internal
