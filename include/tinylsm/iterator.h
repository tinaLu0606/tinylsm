#pragma once

#include <memory>
#include <string_view>

#include "tinylsm/status.h"

namespace tinylsm {

/// Pull-based ordered view of live keys at one DB sequence number.
///
/// Construction captures the MemTable contents and shared SSTable reader
/// ownership, so subsequent Next() calls do not hold the DB state lock.  The
/// iterator itself is not thread-safe.  key() and value() remain valid only
/// until the next Seek(), Next(), or destruction.
class Iterator final {
public:
  ~Iterator();
  Iterator(Iterator&&) noexcept;
  Iterator& operator=(Iterator&&) noexcept;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  /// Positions at the first key not less than target in the creation range.
  Status Seek(std::string_view target);
  [[nodiscard]] bool Valid() const noexcept;
  [[nodiscard]] std::string_view key() const;
  [[nodiscard]] std::string_view value() const;
  Status Next();
  [[nodiscard]] const Status& status() const noexcept;

private:
  friend class DB;
  class Impl;
  explicit Iterator(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace tinylsm
