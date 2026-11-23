// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "glassbox/status.hpp"

namespace gbx {

struct WorkbenchOptions {
  std::string host = "127.0.0.1";
  uint16_t port = 0;  // 0 tries 7331..7340, then an ephemeral port
  bool open_browser = true;
  /// When no trace is available at startup, record the demo workload so the
  /// workbench never opens empty in its default configuration.
  bool auto_record_demo = true;
  /// Explicit trace to open first. Must live under an allowed root.
  std::string initial_trace;
  /// Primary trace directory; also where new demo recordings are written.
  std::string trace_dir = "traces";
  /// Additional directories the UI may list and open traces from.
  std::vector<std::string> extra_trace_dirs;
  std::string app_name = "glassbox";
};

/// The local workbench: HTTP/SSE server, trace registry, demo recorder and
/// browser launcher. The server binds loopback only and requires the random
/// session token printed in `url()`.
///
/// Thread-safety: start/stop must be called from one thread at a time and not
/// concurrently with open_trace for the same path (open_trace may be called
/// while serving). Recording runs on an internal thread.
class Workbench {
 public:
  explicit Workbench(WorkbenchOptions options = {});
  ~Workbench();
  Workbench(const Workbench&) = delete;
  Workbench& operator=(const Workbench&) = delete;

  /// Binds the server, opens or records a trace, starts serving and (when
  /// configured) launches the browser.
  Status start();

  /// Stops serving, ends any live recording and joins worker threads.
  void stop();

  bool running() const;
  uint16_t port() const;
  /// Loopback URL including the session token.
  std::string url() const;
  std::string token() const;

  /// Loads a trace file after validating it is a regular .gbt file under an
  /// allowed root. Thread-safe while serving.
  Status open_trace(std::string_view path);

  Status last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Opens `url` in the default browser using the macOS `open` command.
/// Returns false when the process could not be spawned.
bool open_in_browser(std::string_view url);

}  // namespace gbx
