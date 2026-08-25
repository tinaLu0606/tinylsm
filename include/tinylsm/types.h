#pragma once

#include <string>

namespace tinylsm {

struct Entry {
  std::string key;
  std::string value;

  bool operator==(const Entry&) const = default;
};

} // namespace tinylsm
