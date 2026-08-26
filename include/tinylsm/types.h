#pragma once

#include <string>

namespace tinylsm {

/// A live user key-value pair returned by DB::Scan().
///
/// Deleted records and internal sequence numbers are not exposed.
struct Entry {
  std::string key;
  std::string value;

  bool operator==(const Entry&) const = default;
};

} // namespace tinylsm
