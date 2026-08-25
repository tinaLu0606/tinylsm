#pragma once

#include <cstdint>
#include <string>

namespace tinylsm::internal {

enum class ValueType : std::uint8_t { kValue = 1, kTombstone = 2 };

struct InternalEntry {
  std::string user_key;
  std::uint64_t sequence = 0;
  ValueType type = ValueType::kValue;
  std::string value;

  bool operator==(const InternalEntry&) const = default;
};

struct BytewiseLess {
  using is_transparent = void;
  bool operator()(std::string_view lhs, std::string_view rhs) const { return lhs < rhs; }
};

} // namespace tinylsm::internal
