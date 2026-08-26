#pragma once

#include <cassert>
#include <optional>
#include <utility>

#include "tinylsm/status.h"

namespace tinylsm {

/// Holds either a value of type T or a non-OK Status.
///
/// Check ok() before accessing value(). Calling value() on a failed Result
/// violates the API contract and triggers an assertion in debug builds.
template <typename T> class Result {
public:
  /// Constructs a successful result containing `value`.
  Result(T value) : value_(std::move(value)) {}

  /// Constructs a failed result. `status` must not be OK.
  Result(Status status) : status_(std::move(status)) { assert(!status_.ok()); }

  /// Returns true when this Result contains a value.
  [[nodiscard]] bool ok() const { return status_.ok(); }

  /// Returns OK for a successful Result or its failure status otherwise.
  [[nodiscard]] const Status& status() const { return status_; }

  /// Returns the stored value. The precondition is ok() == true.
  [[nodiscard]] T& value() & {
    assert(ok());
    return *value_;
  }
  [[nodiscard]] const T& value() const& {
    assert(ok());
    return *value_;
  }
  [[nodiscard]] T&& value() && {
    assert(ok());
    return std::move(*value_);
  }

private:
  Status status_;
  std::optional<T> value_;
};

} // namespace tinylsm
