// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Demo application: records the scripted workload into a trace, then opens the
// Glassbox workbench on it. This is the example referenced by the README.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "glassbox/glassbox.hpp"

int main(int argc, char** argv) {
  gbx::DemoOptions demo;
  std::string output = "examples/traces/demo.gbt";
  bool open_browser = true;
  bool serve = true;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) {
      output = argv[++i];
    } else if (arg == "--frames" && i + 1 < argc) {
      demo.frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--no-browser") {
      open_browser = false;
    } else if (arg == "--no-serve") {
      serve = false;
    } else if (arg == "--mach-sampler") {
      demo.use_mach_sampler = true;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "glassbox_demo: records the demo workload then opens the UI\n"
          "  --out <path>     trace output (default examples/traces/demo.gbt)\n"
          "  --frames <n>     frame count (default 90)\n"
          "  --mach-sampler   also sample with Mach thread states\n"
          "  --no-browser     do not open the browser\n"
          "  --no-serve       record only\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }

  const auto result = gbx::record_demo_trace(output, demo);
  if (!result.status.ok()) {
    std::fprintf(stderr, "demo recording failed: %s\n",
                 result.status.describe().c_str());
    return 1;
  }
  std::fprintf(stderr,
               "recorded %s: %llu frames, %llu allocations, trace %.1f ms\n",
               output.c_str(),
               static_cast<unsigned long long>(result.frames_recorded),
               static_cast<unsigned long long>(result.allocations),
               static_cast<double>(result.duration_ns) / 1e6);
  if (!serve) return 0;

  const size_t slash = output.find_last_of('/');
  gbx::WorkbenchOptions options;
  options.initial_trace = output;
  options.trace_dir = slash == std::string::npos ? "." : output.substr(0, slash);
  options.extra_trace_dirs = {options.trace_dir};
  options.open_browser = open_browser;
  options.app_name = "glassbox-demo";

  gbx::Workbench workbench(options);
  const auto status = workbench.start();
  if (!status.ok()) {
    std::fprintf(stderr, "workbench failed: %s\n", status.describe().c_str());
    return 1;
  }
  std::fprintf(stderr, "glassbox: serving %s\n", workbench.url().c_str());
  while (workbench.running()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  workbench.stop();
  return 0;
}
