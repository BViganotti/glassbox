// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

#include "glassbox/recorder.hpp"
#include "glassbox/status.hpp"

namespace gbx {

struct DemoOptions {
  std::string app_name = "glassbox-demo";
  uint32_t seed = 7;
  uint32_t frames = 90;
  bool use_mach_sampler = false;
  bool sleep_spike = true;   // a real 120 ms stall at the spike frame
  uint32_t frame_delay_us = 0;  // optional per-frame pacing for live demos
};

struct DemoResult {
  Status status;
  std::string path;
  uint64_t frames_recorded = 0;
  uint64_t allocations = 0;
  uint64_t leaks = 0;
  uint64_t samples = 0;
  uint64_t duration_ns = 0;
};

/// Records a synthetic-but-real workload into `recorder`: a game-loop style
/// span tree, real allocations (some deliberately leaked until after the
/// trace closes), counters, log lines, snapshots with a drifing inventory
/// count, a latency spike and stack samples.
///
/// The trace is finished by the caller; call record_demo_trace for the
/// one-call variant. The workload allocates real memory and frees the leaked
/// blocks after the recorder finishes, so the trace shows a leak while the
/// process stays clean.
Status run_demo(Recorder& recorder, const DemoOptions& options,
                DemoResult& result);

/// Records the demo workload to `path`.
DemoResult record_demo_trace(const std::string& path, const DemoOptions& options);

}  // namespace gbx
