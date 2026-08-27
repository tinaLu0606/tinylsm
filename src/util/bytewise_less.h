#pragma once

#include <string_view>

namespace tinylsm::internal {

/// Orders byte strings lexicographically and enables heterogeneous lookup.
struct BytewiseLess {
  using is_transparent = void;

  bool operator()(std::string_view lhs, std::string_view rhs) const {
    return lhs < rhs;
  }
};

} // namespace tinylsm::internal
