#pragma once

#include <string>

namespace tinylsm {

/// Coarse error categories for programmatic control flow.
///
/// Status messages are diagnostic text and are not a stable interface. Callers
/// should branch on StatusCode instead of parsing message strings.
enum class StatusCode {
  kOk,              ///< The operation completed successfully.
  kNotFound,        ///< A requested key, file, or database does not exist.
  kInvalidArgument, ///< A caller-provided argument violates the API contract.
  kIOError,         ///< An operating-system or filesystem operation failed.
  kCorruption,      ///< Persistent bytes or internal ordering are invalid.
  kAlreadyClosed,   ///< An operation was attempted on a closed DB or file.
  kNotSupported,    ///< The request exceeds the current TinyLSM feature set.
};

/// The success or failure result of an operation that returns no value.
///
/// Expected database failures are returned as Status values rather than thrown
/// as C++ exceptions. This does not suppress exceptions such as std::bad_alloc.
class Status {
public:
  /// Constructs an OK status.
  Status() = default;
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status Ok() { return {}; }
  static Status NotFound(std::string message) {
    return {StatusCode::kNotFound, std::move(message)};
  }
  static Status InvalidArgument(std::string message) {
    return {StatusCode::kInvalidArgument, std::move(message)};
  }
  static Status IOError(std::string message) {
    return {StatusCode::kIOError, std::move(message)};
  }
  static Status Corruption(std::string message) {
    return {StatusCode::kCorruption, std::move(message)};
  }
  static Status AlreadyClosed(std::string message) {
    return {StatusCode::kAlreadyClosed, std::move(message)};
  }
  static Status NotSupported(std::string message) {
    return {StatusCode::kNotSupported, std::move(message)};
  }

  /// Returns true only for StatusCode::kOk.
  [[nodiscard]] bool ok() const { return code_ == StatusCode::kOk; }

  /// Returns the stable category intended for programmatic checks.
  [[nodiscard]] StatusCode code() const { return code_; }

  /// Returns diagnostic text intended for humans, logs, and tests.
  [[nodiscard]] const std::string& message() const { return message_; }

private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

} // namespace tinylsm
