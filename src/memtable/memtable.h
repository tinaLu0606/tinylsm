#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "model/internal_entry.h"
#include "tinylsm/result.h"

namespace tinylsm::internal {

class MemTable {
public:
  Status Apply(InternalEntry entry);
  [[nodiscard]] Result<InternalEntry> Get(std::string_view key) const;
  [[nodiscard]] std::vector<InternalEntry> Scan(std::string_view begin, std::string_view end) const;
  [[nodiscard]] std::size_t ApproximateMemoryUsage() const { return bytes_; }
  [[nodiscard]] bool Empty() const { return entries_.empty(); }
  void Clear();

private:
  std::map<std::string, InternalEntry, BytewiseLess> entries_;
  std::size_t bytes_ = 0;
};

} // namespace tinylsm::internal
