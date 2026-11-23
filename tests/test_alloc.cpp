// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/alloc.hpp"

#include <unistd.h>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "glassbox/trace_io.hpp"
#include "test_framework.hpp"

namespace {

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-alloc-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  std::filesystem::remove(dir / name);
  return (dir / name).string();
}

}  // namespace

TEST(source_loc_format) {
  gbx::SourceLoc loc{"engine.cpp", 42, "update()"};
  CHECK_EQ(loc.to_string(), std::string("engine.cpp:42"));
  gbx::SourceLoc no_line{"module.dylib", 0, ""};
  CHECK_EQ(no_line.to_string(), std::string("module.dylib"));
  gbx::SourceLoc unknown{};
  CHECK_EQ(unknown.to_string(), std::string("<unknown>"));
}

TEST(object_registry_basics) {
  gbx::ObjectRegistry registry(4);
  gbx::ObjectRegistry::Entry a;
  a.ptr = 0x1000;
  a.size = 100;
  a.site = "a.cpp:1";
  a.klass = "A";
  gbx::ObjectRegistry::Entry child;
  child.ptr = 0x2000;
  child.size = 50;
  child.owner = 0x1000;
  child.site = "b.cpp:2";
  child.klass = "B";
  CHECK(registry.add(a));
  CHECK(registry.add(child));
  CHECK_EQ(registry.size(), size_t{2});
  CHECK_EQ(registry.live_bytes(), uint64_t{150});
  CHECK(registry.find(0x1000).has_value());
  CHECK_EQ(registry.find(0x1000)->klass, std::string("A"));
  CHECK(!registry.find(0x9999).has_value());
  CHECK_EQ(registry.children(0x1000).size(), size_t{1});
  CHECK_EQ(registry.roots().size(), size_t{1});
  CHECK_EQ(registry.roots()[0].ptr, uint64_t{0x1000});
  CHECK(registry.remove(0x1000));
  CHECK(!registry.remove(0x1000));
  CHECK_EQ(registry.live_bytes(), uint64_t{50});
  CHECK(!registry.add({}));
}

TEST(object_registry_bounded) {
  gbx::ObjectRegistry registry(2);
  gbx::ObjectRegistry::Entry e;
  e.ptr = 1;
  e.size = 10;
  CHECK(registry.add(e));
  e.ptr = 2;
  CHECK(registry.add(e));
  e.ptr = 3;
  CHECK(!registry.add(e));
  CHECK_EQ(registry.size(), size_t{2});
  CHECK_EQ(registry.overflow_count(), uint64_t{1});
  // Replacing a known pointer does not count as an overflow.
  e.ptr = 1;
  e.size = 20;
  CHECK(registry.add(e));
  CHECK_EQ(registry.live_bytes(), uint64_t{30});
}

TEST(allocation_tracker_emits_and_aggregates) {
  const std::string path = temp_path("tracker.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  {
    gbx::AllocationTracker tracker(rec, 100);
    tracker.alloc(reinterpret_cast<void*>(0x100), 128, {"heap.cpp", 10, "make"});
    tracker.alloc(reinterpret_cast<void*>(0x200), 256, {"heap.cpp", 10, "make"});
    tracker.alloc(reinterpret_cast<void*>(0x300), 64, {"pool.cpp", 3, "grow"});
    tracker.free(reinterpret_cast<void*>(0x100));
    CHECK(tracker.is_live(reinterpret_cast<void*>(0x200)));
    CHECK(!tracker.is_live(reinterpret_cast<void*>(0x100)));
    const auto summary = tracker.summary();
    CHECK_EQ(summary.live_objects, uint64_t{2});
    CHECK_EQ(summary.live_bytes, uint64_t{320});
    const auto sites = tracker.sites(10);
    CHECK_EQ(sites.size(), size_t{2});
    CHECK_EQ(sites[0].site, std::string("heap.cpp:10"));
    CHECK_EQ(sites[0].bytes, uint64_t{256});
    CHECK_EQ(sites[0].count, uint64_t{1});
  }
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  size_t allocs = 0;
  size_t frees = 0;
  for (const auto& record : trace.records) {
    if (record.type == gbx::RecordType::Alloc) {
      ++allocs;
      CHECK(!trace.str_field(record, "site").empty());
    } else if (record.type == gbx::RecordType::Free) {
      ++frees;
    }
  }
  CHECK_EQ(allocs, size_t{3});
  CHECK_EQ(frees, size_t{1});
}

TEST(allocation_tracker_free_tracked_ignores_unknown) {
  const std::string path = temp_path("tracked.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  gbx::AllocationTracker tracker(rec, 100);
  tracker.alloc(reinterpret_cast<void*>(0xabc), 32, {"x.cpp", 1, "x"});
  CHECK(tracker.free_tracked(reinterpret_cast<void*>(0xabc)));
  CHECK(!tracker.free_tracked(reinterpret_cast<void*>(0xabc)));
  CHECK(!tracker.free_tracked(reinterpret_cast<void*>(0xdef)));
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  size_t frees = 0;
  for (const auto& record : trace.records) {
    if (record.type == gbx::RecordType::Free) ++frees;
  }
  CHECK_EQ(frees, size_t{1});
}

TEST(allocation_tracker_threaded) {
  const std::string path = temp_path("threaded.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  gbx::AllocationTracker tracker(rec, 10000);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&tracker, t] {
      for (int i = 0; i < 100; ++i) {
        const auto ptr = reinterpret_cast<void*>(
            static_cast<uintptr_t>(0x10000 + t * 1000 + i));
        tracker.alloc(ptr, 16, {"thread.cpp", __LINE__, "worker"});
        tracker.free(ptr);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK_EQ(tracker.summary().live_objects, uint64_t{0});
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  size_t allocs = 0;
  size_t frees = 0;
  for (const auto& record : trace.records) {
    if (record.type == gbx::RecordType::Alloc) ++allocs;
    if (record.type == gbx::RecordType::Free) ++frees;
  }
  CHECK_EQ(allocs, size_t{400});
  CHECK_EQ(frees, size_t{400});
}
