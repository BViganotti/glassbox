// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/status.hpp"

namespace gbx {

std::string_view to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::Truncated: return "truncated";
    case ErrorCode::Corrupt: return "corrupt";
    case ErrorCode::LimitExceeded: return "limit_exceeded";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::PermissionDenied: return "permission_denied";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

std::string Status::describe() const {
  if (ok()) return "ok";
  std::string out(to_string(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace gbx
