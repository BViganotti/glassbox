// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Verifies the opt-in global allocation hooks. This binary links
// glassbox_hooks, so every allocation in this process (including the test
// harness) is routed through the intercepted operators. Sanitizer builds skip
// this target because ASan/TSan own the allocator.
#include "glassbox/alloc_hooks.hpp"

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include "glassbox/recorder.hpp"
#include "glassbox/trace_io.hpp"
#include "test_framework.hpp"

namespace {

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-hooks-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  std::filesystem::remove(dir / name);
  return (dir / name).string();
}

struct Counts {
  size_t allocs = 0;
  size_t frees = 0;
  uint64_t bytes = 0;
  bool saw_site = false;
};

Counts count_trace(const gbx::Trace& trace) {
  Counts counts;
  for (const auto& record : trace.records) {
    if (record.type == gbx::RecordType::Alloc) {
      ++counts.allocs;
      counts.bytes += static_cast<uint64_t>(
          trace.int_field(record, "size", 0));
      if (!trace.str_field(record, "site").empty()) counts.saw_site = true;
    } else if (record.type == gbx::RecordType::Free) {
      ++counts.frees;
    }
  }
  return counts;
}

}  // namespace

TEST(hooks_validation) {
  gbx::HookConfig config;
  auto status = gbx::install_allocation_hooks(config);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::InvalidArgument);

  const std::string path = temp_path("validation.gbt");
  gbx::Recorder rec;
  config.recorder = &rec;
  status = gbx::install_allocation_hooks(config);
  CHECK(!status.ok());  // recorder not started
  CHECK(rec.start(path).ok());
  config.sample_every = 0;
  status = gbx::install_allocation_hooks(config);
  CHECK(!status.ok());
  config.sample_every = 1;
  CHECK(gbx::install_allocation_hooks(config).ok());
  CHECK(gbx::allocation_hooks_installed());
  CHECK(!gbx::install_allocation_hooks(config).ok());  // already installed
  gbx::uninstall_allocation_hooks();
  CHECK(!gbx::allocation_hooks_installed());
  CHECK(rec.finish().ok());
}

TEST(hooks_capture_heap_allocations) {
  const std::string path = temp_path("heap.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  gbx::HookConfig config;
  config.recorder = &rec;
  config.sample_every = 1;
  CHECK(gbx::install_allocation_hooks(config).ok());

  void* small = ::operator new(128);
  ::operator delete(small);
  void* large = ::operator new(4096);
  ::operator delete(large);
  // A deliberate leak, visible as Alloc without Free.
  volatile void* leaked = ::operator new(512);
  (void)leaked;

  gbx::uninstall_allocation_hooks();
  CHECK(rec.finish().ok());

  gbx::Trace trace;
  auto status = gbx::load_trace(path, trace);
  CHECK_MSG(status.ok(), status.describe());
  const auto counts = count_trace(trace);
  CHECK_GE(counts.allocs, size_t{3});
  CHECK_GE(counts.frees, size_t{2});
  CHECK_GE(counts.bytes, uint64_t{128 + 4096 + 512});
  CHECK(counts.saw_site);
}

TEST(hooks_stop_after_uninstall) {
  const std::string path = temp_path("uninstall.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  gbx::HookConfig config;
  config.recorder = &rec;
  CHECK(gbx::install_allocation_hooks(config).ok());
  void* before = ::operator new(64);
  ::operator delete(before);
  gbx::uninstall_allocation_hooks();
  void* after = ::operator new(64);
  ::operator delete(after);
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  const auto counts = count_trace(trace);
  CHECK_GE(counts.allocs, size_t{1});
  CHECK_GE(counts.frees, size_t{1});
}

TEST(hooks_sampling_reduces_records) {
  const std::string unsampled = temp_path("unsampled.gbt");
  {
    gbx::Recorder rec;
    CHECK(rec.start(unsampled).ok());
    gbx::HookConfig config;
    config.recorder = &rec;
    CHECK(gbx::install_allocation_hooks(config).ok());
    std::vector<void*> ptrs;
    for (int i = 0; i < 64; ++i) ptrs.push_back(::operator new(32));
    for (void* p : ptrs) ::operator delete(p);
    gbx::uninstall_allocation_hooks();
    CHECK(rec.finish().ok());
  }
  const std::string sampled = temp_path("sampled.gbt");
  {
    gbx::Recorder rec;
    CHECK(rec.start(sampled).ok());
    gbx::HookConfig config;
    config.recorder = &rec;
    config.sample_every = 8;
    CHECK(gbx::install_allocation_hooks(config).ok());
    std::vector<void*> ptrs;
    for (int i = 0; i < 64; ++i) ptrs.push_back(::operator new(32));
    for (void* p : ptrs) ::operator delete(p);
    gbx::uninstall_allocation_hooks();
    CHECK(rec.finish().ok());
  }
  gbx::Trace a;
  gbx::Trace b;
  CHECK(gbx::load_trace(unsampled, a).ok());
  CHECK(gbx::load_trace(sampled, b).ok());
  CHECK_GT(count_trace(a).allocs, count_trace(b).allocs);
}

TEST(hooks_min_size_filters_small_allocations) {
  const std::string path = temp_path("min-size.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  gbx::HookConfig config;
  config.recorder = &rec;
  config.min_size = 512;
  CHECK(gbx::install_allocation_hooks(config).ok());
  void* tiny = ::operator new(32);
  ::operator delete(tiny);
  void* medium = ::operator new(1024);
  ::operator delete(medium);
  gbx::uninstall_allocation_hooks();
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  size_t small_allocs = 0;
  size_t large_allocs = 0;
  for (const auto& record : trace.records) {
    if (record.type != gbx::RecordType::Alloc) continue;
    const auto size = trace.int_field(record, "size", 0);
    if (size < 512) ++small_allocs;
    if (size >= 1024) ++large_allocs;
  }
  CHECK_EQ(small_allocs, size_t{0});
  CHECK_GE(large_allocs, size_t{1});
}
