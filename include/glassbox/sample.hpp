// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "glassbox/recorder.hpp"
#include "glassbox/status.hpp"

namespace gbx {

struct SymbolInfo {
  std::string name;    // demangled when possible
  std::string module;  // image path, possibly empty
  uint64_t address = 0;
  uint64_t offset = 0;  // address - module base
};

/// Resolves an address with dladdr and demangles C++ names via the C++ ABI.
/// Never throws; unknown addresses yield "<unknown>".
SymbolInfo symbolize_address(uintptr_t address);
SymbolInfo symbolize_pointer(const void* pointer);

/// Sanitizes and truncates a symbol name for trace storage.
std::string short_symbol(std::string_view name, size_t limit = 160);

/// Captures and records manual stack samples from the calling thread.
///
/// The capture walks the frame-pointer chain. It is most accurate in builds
/// compiled with frame pointers (the default on arm64 macOS and for this
/// project). Missing or invalid frames simply end the walk.
class ManualSampler {
 public:
  explicit ManualSampler(Recorder& recorder) : recorder_(recorder) {}

  /// Walks the current stack (at most `max_frames`) and records a sample with
  /// `phase` as the outermost frame.
  void capture(std::string_view phase, size_t max_frames = 24);

  /// Records a caller-supplied stack, outermost frame first.
  void record(std::span<const std::string_view> frames, double weight = 1.0) {
    recorder_.sample(frames, weight);
  }

 private:
  Recorder& recorder_;
};

struct MachSamplerConfig {
  Recorder* recorder = nullptr;
  uint32_t interval_us = 2000;  // clamp: 100 .. 1,000,000
  size_t max_frames = 24;        // clamp: 1 .. 64
  uint32_t max_threads = 32;     // clamp: 1 .. 256
  bool capture_frame_pointers = true;
};

struct MachSamplerStats {
  uint64_t samples = 0;
  uint64_t thread_states = 0;
  uint64_t frames = 0;
  uint64_t failed_states = 0;
  uint64_t errors = 0;
};

/// Periodic sampler for this process using Mach thread states. It captures the
/// program counter of every thread and, when the stack can be validated,
/// walks the frame-pointer chain. Sampling other threads without suspending
/// them is inherently approximate; addresses are read through
/// mach_vm_read_overwrite so an invalid frame can never crash the sampler.
///
/// Thread-safety: start/stop must be serialized by the caller. The recorder
/// must remain recording while the sampler runs (stop it first).
class MachSampler {
 public:
  MachSampler();
  ~MachSampler();
  MachSampler(const MachSampler&) = delete;
  MachSampler& operator=(const MachSampler&) = delete;

  Status start(const MachSamplerConfig& config);
  void stop();
  bool running() const;

  MachSamplerStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gbx
