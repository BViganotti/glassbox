// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Small platform seam for the POSIX/macOS APIs used by Glassbox. Keeping the
// differences here lets the recorder, sampler, HTTP server and launcher build
// on both macOS and Linux.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gbx::platform {

/// Names the calling thread when the platform supports it.
void set_current_thread_name(std::string_view name);

/// Returns the calling thread's name, or an empty string when unknown.
std::string current_thread_name();

/// Fills `bytes` with cryptographically strong random data. Never throws;
/// on failure the buffer is zero-filled (callers treat the token as weak and
/// the launcher would still be loopback-only).
void random_bytes(unsigned char* bytes, size_t size);

/// Enables no-SIGPIPE behavior for a socket where the OS supports it.
void set_no_sigpipe(int fd);

/// Sends bytes without raising SIGPIPE. Returns the raw send result.
long send_bytes(int fd, const void* data, size_t size);

/// The system command used to open a URL in the default browser.
const char* browser_command();

}  // namespace gbx::platform
