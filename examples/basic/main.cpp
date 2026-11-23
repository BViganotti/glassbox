// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Minimal embedding example: spans, logs, a counter and a tracked allocation.
#include <cstdio>
#include <memory>
#include <string>

#include "glassbox/glassbox.hpp"

int main() {
  gbx::Recorder recorder;
  const auto status = recorder.start("traces/basic.gbt");
  if (!status.ok()) {
    std::fprintf(stderr, "cannot start recorder: %s\n",
                 status.describe().c_str());
    return 1;
  }
  gbx::AllocationTracker tracker(recorder);
  recorder.thread_name("main");
  {
    auto span = recorder.begin_span("ParseShot", "cpu");
    recorder.log(gbx::LogLevel::Info, "starting parse", "example");
    auto* buffer = new char[4096];
    tracker.alloc(buffer, 4096, {"example.cpp", 18, "main"}, "Buffer");
    recorder.counter("bytes_parsed", 4096, "B");
    recorder.snapshot({{"shots", 1}, {"errors", 0}});
    recorder.log(gbx::LogLevel::Info, "parse complete", "example");
    tracker.free(buffer);
    delete[] buffer;
    span.counter("elapsed", 0.4);
  }
  const auto finished = recorder.finish();
  if (!finished.ok()) {
    std::fprintf(stderr, "recording failed: %s\n", finished.describe().c_str());
    return 1;
  }
  const auto stats = recorder.stats();
  std::printf("wrote traces/basic.gbt: %llu records\n",
              static_cast<unsigned long long>(stats.records_written));
  return 0;
}
