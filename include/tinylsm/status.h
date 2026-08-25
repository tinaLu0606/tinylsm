#pragma once

#include <string>

namespace tinylsm {

enum class StatusCode {
  kOk,
  kNotFound,
  kInvalidArgument,
  kIOError,
  kCorruption,
  kAlreadyClosed,
  kNotSupported,
};

class Status {
public:
  Status() = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status Ok() { return {}; }
  static Status NotFound(std::string message) {
    return {StatusCode::kNotFound, std::move(message)};
  }
  static Status InvalidArgument(std::string message) {
    return {StatusCode::kInvalidArgument, std::move(message)};
  }
  static Status IOError(std::string message) { return {StatusCode::kIOError, std::move(message)}; }
  static Status Corruption(std::string message) {
    return {StatusCode::kCorruption, std::move(message)};
  }
  static Status AlreadyClosed(std::string message) {
    return {StatusCode::kAlreadyClosed, std::move(message)};
  }
  static Status NotSupported(std::string message) {
    return {StatusCode::kNotSupported, std::move(message)};
  }

  [[nodiscard]] bool ok() const { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const { return code_; }
  [[nodiscard]] const std::string& message() const { return message_; }

private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

} // namespace tinylsm
