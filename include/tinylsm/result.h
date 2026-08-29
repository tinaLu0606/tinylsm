#pragma once

#include <cassert>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "tinylsm/status.h"

namespace tinylsm {

/// Thrown when value() is called on a failed Result.
class BadResultAccess final : public std::exception {
public:
  explicit BadResultAccess(Status status)
      : status_(std::move(status)), what_("bad Result access: " + status_.ToString()) {}

  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] const char* what() const noexcept override { return what_.c_str(); }

private:
  Status status_;
  std::string what_;
};

/// Holds either a value of type T or a non-OK Status.
///
/// Check ok() before using operator* or operator->. value() performs a checked
/// access and throws BadResultAccess when this Result contains an error.
template <typename T> class [[nodiscard]] Result {
public:
  /// Constructs a successful result containing `value`.
  Result(T value) : value_(std::move(value)) {}

  /// Constructs a failed result. Passing an OK status is a contract violation.
  Result(Status status) : status_(ValidateError(std::move(status))) {}

  /// Returns true when this Result contains a value.
  [[nodiscard]] bool ok() const { return status_.ok(); }

  /// Returns OK for a successful Result or its failure status otherwise.
  [[nodiscard]] const Status& status() const { return status_; }

  /// Returns the stored value or throws BadResultAccess.
  [[nodiscard]] T& value() & {
    CheckHasValue();
    return *value_;
  }
  [[nodiscard]] const T& value() const& {
    CheckHasValue();
    return *value_;
  }
  [[nodiscard]] T&& value() && {
    CheckHasValue();
    return std::move(*value_);
  }

  /// Pointer-like access after the caller has checked ok().
  [[nodiscard]] T& operator*() & {
    assert(ok());
    return *value_;
  }
  [[nodiscard]] const T& operator*() const& {
    assert(ok());
    return *value_;
  }
  [[nodiscard]] T&& operator*() && {
    assert(ok());
    return std::move(*value_);
  }
  [[nodiscard]] T* operator->() {
    assert(ok());
    return std::addressof(*value_);
  }
  [[nodiscard]] const T* operator->() const {
    assert(ok());
    return std::addressof(*value_);
  }

private:
  static Status ValidateError(Status status) {
    if (status.ok())
      throw std::invalid_argument("failed Result requires a non-OK Status");
    return status;
  }

  void CheckHasValue() const {
    if (!ok())
      throw BadResultAccess(status_);
  }

  Status status_;
  std::optional<T> value_;
};

} // namespace tinylsm
