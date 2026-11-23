// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "glassbox/trace.hpp"
#include "ui/http.hpp"

namespace gbx::ui {

struct TraceSnapshot {
  std::shared_ptr<const Trace> trace;
  std::string path;
  std::string name;
  uint64_t bytes = 0;
  int64_t mtime = 0;
};

using TraceGetter = std::function<TraceSnapshot()>;
/// Returns true when a request may proceed; on false the guard has already
/// filled in a response (usually 403).
using Guard = std::function<bool(const HttpRequest&, HttpResponse&)>;

/// Registers /api/meta, /api/events, /api/timeline, analysis, query and diff
/// endpoints. All routes go through `guard`.
void register_trace_routes(HttpServer& server, TraceGetter getter, Guard guard);

}  // namespace gbx::ui
