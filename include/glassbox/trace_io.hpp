// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "glassbox/status.hpp"
#include "glassbox/trace.hpp"

namespace gbx {

/// Tuning for writing trace files.
struct SaveOptions {
  size_t chunk_bytes = 1u << 20;
  uint32_t chunk_records = 50000;
  uint32_t chunk_strings = 4096;
  bool sync_on_close = false;
};

/// Options for loading untrusted trace files.
struct LoadOptions {
  TraceLimits limits;
  bool recover_truncated = true;
  bool verify_checksums = true;
  /// `load_trace` streams chunks from disk by default (peak memory is the
  /// decoded model plus one chunk). `load_trace_bytes` is unaffected.
  bool streaming = true;
};

/// Builds metadata for the current process. `exe_path` may be empty when the
/// caller cannot determine it.
TraceMeta current_meta(std::string_view app_name, std::string_view app_version,
                       std::string_view exe_path = {});

/// Serializes a finalized (or unsorted) trace to disk. On failure the partial
/// file has no footer and is reported as truncated when read back.
Status save_trace(const std::string& path, const Trace& trace,
                  const SaveOptions& options = {});

/// Loads and validates a trace file. Diagnostics are stored on the trace.
Status load_trace(const std::string& path, Trace& out,
                  const LoadOptions& options = {});

/// Loads a trace from memory (used by fuzzing and HTTP imports).
Status load_trace_bytes(std::span<const uint8_t> bytes, Trace& out,
                        const LoadOptions& options = {},
                        const std::string& source = "<memory>");

}  // namespace gbx
