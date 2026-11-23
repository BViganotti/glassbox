// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

TEST(heap_at_reconstructs_live_objects) {
  gbx::test::AnalysisFixture f;
  f.alloc(10, 0x1000, 100, "mesh.cpp:10", "Mesh");
  f.alloc(20, 0x2000, 50, "tex.cpp:20", "Texture");
  f.free(30, 0x1000);
  f.alloc(40, 0x3000, 300, "mesh.cpp:10", "Mesh", 0x2000);
  f.finalize();

  auto empty = gbx::heap_at(f.trace, 5);
  CHECK_EQ(empty.total_bytes, uint64_t{0});
  CHECK_EQ(empty.object_count, uint64_t{0});

  auto at25 = gbx::heap_at(f.trace, 25);
  CHECK_EQ(at25.total_bytes, uint64_t{150});
  CHECK_EQ(at25.object_count, uint64_t{2});
  CHECK_EQ(at25.alloc_count, uint64_t{2});
  CHECK_EQ(at25.free_count, uint64_t{0});
  CHECK_EQ(at25.sites.size(), size_t{2});
  CHECK_EQ(at25.sites[0].site, std::string("mesh.cpp:10"));
  CHECK_EQ(at25.sites[0].live_bytes, uint64_t{100});
  CHECK_EQ(at25.objects[0].ptr, uint64_t{0x1000});
  CHECK_EQ(at25.objects[0].age_ns, uint64_t{15});

  auto at35 = gbx::heap_at(f.trace, 35);
  CHECK_EQ(at35.total_bytes, uint64_t{50});
  CHECK_EQ(at35.object_count, uint64_t{1});
  CHECK_EQ(at35.free_count, uint64_t{1});
  CHECK_EQ(at35.peak_bytes, uint64_t{150});
  CHECK_EQ(at35.peak_ts, uint64_t{20});

  auto at100 = gbx::heap_at(f.trace, 100);
  CHECK_EQ(at100.total_bytes, uint64_t{350});
  CHECK_EQ(at100.object_count, uint64_t{2});
  CHECK_EQ(at100.objects[0].size, uint64_t{300});
  CHECK_EQ(at100.objects[0].owner, uint64_t{0x2000});
  CHECK_EQ(at100.objects_truncated, size_t{0});
}

TEST(heap_at_handles_reallocation_and_unknown_free) {
  gbx::test::AnalysisFixture f;
  f.alloc(10, 0x1000, 100, "a", "A");
  f.alloc(20, 0x1000, 200, "a", "A");  // same pointer, implicit reallocation
  f.free(30, 0x9999);                   // unknown pointer
  f.free(40, 0x1000);
  f.finalize();
  auto at25 = gbx::heap_at(f.trace, 25);
  CHECK_EQ(at25.total_bytes, uint64_t{200});
  CHECK_EQ(at25.object_count, uint64_t{1});
  CHECK_EQ(at25.free_count, uint64_t{1});  // implicit free of the first block
  CHECK_EQ(at25.unknown_frees, uint64_t{0});
  auto at35 = gbx::heap_at(f.trace, 35);
  CHECK_EQ(at35.total_bytes, uint64_t{200});
  CHECK_EQ(at35.unknown_frees, uint64_t{1});
  auto at50 = gbx::heap_at(f.trace, 50);
  CHECK_EQ(at50.total_bytes, uint64_t{0});
  CHECK_EQ(at50.free_count, uint64_t{2});
  CHECK_EQ(at50.peak_bytes, uint64_t{200});
}

TEST(heap_sites_are_deterministic_and_capped) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0x10, 10, "b.cpp:2", "B");
  f.alloc(2, 0x20, 20, "a.cpp:1", "A");
  f.alloc(3, 0x30, 20, "a.cpp:1", "A");
  f.finalize();
  auto heap = gbx::heap_at(f.trace, 10);
  CHECK_EQ(heap.sites.size(), size_t{2});
  CHECK_EQ(heap.sites[0].site, std::string("a.cpp:1"));
  CHECK_EQ(heap.sites[0].live_bytes, uint64_t{40});
  CHECK_EQ(heap.sites[0].live_count, uint64_t{2});
  gbx::HeapOptions options;
  options.max_sites = 1;
  options.max_objects = 2;
  auto capped = gbx::heap_at(f.trace, 10, options);
  CHECK_EQ(capped.sites.size(), size_t{1});
  CHECK_EQ(capped.sites_truncated, size_t{1});
  CHECK_EQ(capped.objects.size(), size_t{2});
  CHECK_EQ(capped.objects_truncated, size_t{1});
}

TEST(heap_live_allocations_sorted) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0x10, 10, "s", "K");
  f.alloc(2, 0x20, 30, "s", "K");
  f.alloc(3, 0x30, 20, "s", "K");
  f.free(4, 0x30);
  f.finalize();
  const auto live = gbx::live_allocations(f.trace, 10);
  CHECK_EQ(live.size(), size_t{2});
  CHECK_EQ(live[0].ptr, uint64_t{0x20});
  CHECK_EQ(live[1].ptr, uint64_t{0x10});
  const auto capped = gbx::live_allocations(f.trace, 10, 1);
  CHECK_EQ(capped.size(), size_t{1});
}

TEST(leak_candidates_group_and_rank) {
  gbx::test::AnalysisFixture f;
  f.alloc(10, 0x1000, 400, "leak.cpp:1", "Leak");
  f.alloc(20, 0x2000, 400, "leak.cpp:1", "Leak");
  f.alloc(90, 0x3000, 100, "late.cpp:9", "Late");
  f.alloc(30, 0x4000, 50, "freed.cpp:2", "Freed");
  f.free(40, 0x4000);
  f.marker(100, "end");
  f.finalize();
  const auto leaks = gbx::leak_candidates(f.trace, 10);
  CHECK_EQ(leaks.size(), size_t{2});
  CHECK_EQ(leaks[0].site, std::string("leak.cpp:1"));
  CHECK_EQ(leaks[0].bytes, uint64_t{800});
  CHECK_EQ(leaks[0].count, uint64_t{2});
  CHECK_EQ(leaks[0].oldest_ts, uint64_t{10});
  CHECK_EQ(leaks[0].newest_ts, uint64_t{20});
  CHECK_EQ(leaks[1].site, std::string("late.cpp:9"));
  CHECK_GT(leaks[0].score, leaks[1].score);
  const auto limited = gbx::leak_candidates(f.trace, 1);
  CHECK_EQ(limited.size(), size_t{1});
}

TEST(heap_at_malformed_records_are_skipped) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0, 100, "zero", "Zero");  // null pointer
  f.scratch.push_back({f.str("ptr"), gbx::Value::integer(0x1000)});
  f.scratch.push_back({f.str("size"), gbx::Value::integer(-5)});
  f.emit(2, gbx::RecordType::Alloc);
  f.alloc(3, 0x2000, 10, "good", "Good");
  f.finalize();
  auto heap = gbx::heap_at(f.trace, 10);
  CHECK_EQ(heap.object_count, uint64_t{1});
  CHECK_EQ(heap.total_bytes, uint64_t{10});
}
