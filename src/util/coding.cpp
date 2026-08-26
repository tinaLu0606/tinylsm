#include "util/coding.h"

namespace tinylsm::internal {

void PutFixed16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value));
  out.push_back(static_cast<char>(value >> 8U));
}
void PutFixed32(std::string& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>(value >> shift));
  }
}
void PutFixed64(std::string& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>(value >> shift));
  }
}
bool GetFixed16(std::span<const std::byte> in, std::size_t offset,
                std::uint16_t& value) {
  if (offset > in.size() || in.size() - offset < 2)
    return false;
  value =
      std::to_integer<std::uint8_t>(in[offset]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[offset + 1])) << 8U);
  return true;
}
bool GetFixed32(std::span<const std::byte> in, std::size_t offset,
                std::uint32_t& value) {
  if (offset > in.size() || in.size() - offset < 4)
    return false;
  value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[offset + i]))
             << (8U * i);
  }
  return true;
}
bool GetFixed64(std::span<const std::byte> in, std::size_t offset,
                std::uint64_t& value) {
  if (offset > in.size() || in.size() - offset < 8)
    return false;
  value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[offset + i]))
             << (8U * i);
  }
  return true;
}
std::span<const std::byte> AsBytes(std::string_view input) {
  return {reinterpret_cast<const std::byte*>(input.data()), input.size()};
}

} // namespace tinylsm::internal
