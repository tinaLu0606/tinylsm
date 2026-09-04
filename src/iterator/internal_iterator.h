#pragma once

#include <memory>
#include <vector>

#include "model/internal_entry.h"
#include "tinylsm/result.h"

namespace tinylsm::internal {

/// Internal forward iterator shared by MemTable and SSTable read paths.
class InternalIterator {
public:
  virtual ~InternalIterator() = default;

  [[nodiscard]] virtual bool Valid() const noexcept = 0;
  [[nodiscard]] virtual const InternalEntry& entry() const = 0;
  virtual Status Next() = 0;
  [[nodiscard]] virtual const Status& status() const noexcept = 0;
};

/// Owns all inputs and exposes the newest sequence for each byte-wise key.
Result<std::unique_ptr<InternalIterator>>
NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> inputs);

} // namespace tinylsm::internal
