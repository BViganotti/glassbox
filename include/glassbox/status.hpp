// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace gbx {

/// Machine-readable outcome of an operation. Glassbox uses status codes rather
/// than exceptions across its public API so that malformed input can never
/// escape as an uncaught throw.
enum class ErrorCode {
  Ok = 0,
  InvalidArgument,
  IoError,
  Truncated,
  Corrupt,
  LimitExceeded,
  NotFound,
  Busy,
  Unsupported,
  PermissionDenied,
  Internal,
};

std::string_view to_string(ErrorCode code);

/// A status value: an error code plus a human-readable message. The default
/// constructed status is success.
class Status {
 public:
  Status() = default;
  Status(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status success() { return {}; }
  static Status error(ErrorCode code, std::string message) {
    return {code, std::move(message)};
  }

  bool ok() const { return code_ == ErrorCode::Ok; }
  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }

  /// Renders "code: message" for logs and diagnostics.
  std::string describe() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
};

/// Convenience helper for propagating a failure while adding context.
inline Status wrap(Status status, std::string_view context) {
  if (status.ok()) return status;
  std::string message(context);
  if (!status.message().empty()) {
    message += ": ";
    message += status.message();
  }
  return Status(status.code(), std::move(message));
}

}  // namespace gbx
