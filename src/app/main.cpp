// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Glassbox launcher. Starts the local workbench and opens the default browser.
// No terminal interaction is required; flags exist for scripting tests.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "glassbox/glassbox.hpp"

namespace {

std::atomic<bool> g_interrupted{false};

void on_signal(int) { g_interrupted.store(true); }

void print_usage() {
  std::printf(
      "glassbox %s - time-travel inspector for native C++ applications\n"
      "\n"
      "Usage: glassbox [options]\n"
      "  --trace <path>       open a specific .gbt trace\n"
      "  --trace-dir <dir>    primary trace directory (default: traces)\n"
      "  --port <n>           loopback port (default: first free of 7331..7340)\n"
      "  --no-browser         do not launch the browser\n"
      "  --no-record          do not record the demo when no trace exists\n"
      "  --help               show this message\n",
      GBX_VERSION);
}

}  // namespace

int main(int argc, char** argv) {
  gbx::WorkbenchOptions options;
  options.trace_dir = "traces";
  options.extra_trace_dirs = {"examples/traces"};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--trace") {
      if (!value(options.initial_trace)) {
        std::fprintf(stderr, "--trace requires a path\n");
        return 2;
      }
    } else if (arg == "--trace-dir") {
      if (!value(options.trace_dir)) {
        std::fprintf(stderr, "--trace-dir requires a path\n");
        return 2;
      }
    } else if (arg == "--port") {
      std::string port;
      if (!value(port)) {
        std::fprintf(stderr, "--port requires a number\n");
        return 2;
      }
      options.port = static_cast<uint16_t>(std::strtoul(port.c_str(), nullptr, 10));
    } else if (arg == "--no-browser") {
      options.open_browser = false;
    } else if (arg == "--no-record") {
      options.auto_record_demo = false;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  gbx::Workbench workbench(options);
  const auto status = workbench.start();
  if (!status.ok()) {
    std::fprintf(stderr, "glassbox: %s\n", status.describe().c_str());
    return 1;
  }
  std::fprintf(stderr, "glassbox: serving %s\n", workbench.url().c_str());
  while (workbench.running() && !g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  workbench.stop();
  return 0;
}
