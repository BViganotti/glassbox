// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

TEST(timeline_buckets_and_totals) {
  gbx::test::AnalysisFixture f;
  const uint32_t a = f.begin_span("Root", 0);
  const uint32_t b = f.begin_span("Child", 10, a);
  f.alloc(10, 0x1000, 100, "a.cpp:1", "Mesh");
  f.log(20, gbx::LogLevel::Error, "boom");
  f.sample(25, {"main", "Update"});
  f.alloc(30, 0x2000, 50, "a.cpp:2", "Texture");
  f.free(40, 0x1000);
  f.end_span(b, 50, 1, a);
  f.end_span(a, 100);
  f.finalize();

  const auto timeline = gbx::build_timeline(f.trace, 4);
  CHECK_EQ(timeline.buckets.size(), size_t{4});
  CHECK_EQ(timeline.bucket_width, uint64_t{25});
  CHECK_EQ(timeline.buckets[0].begin_ts, uint64_t{0});
  CHECK_EQ(timeline.buckets[3].end_ts, uint64_t{100});
  CHECK_EQ(timeline.buckets[0].alloc_bytes, uint64_t{100});
  CHECK_EQ(timeline.buckets[0].alloc_count, uint64_t{1});
  CHECK_EQ(timeline.buckets[0].error_count, uint64_t{1});
  CHECK_EQ(timeline.buckets[0].log_count, uint64_t{1});
  CHECK_EQ(timeline.buckets[0].sample_count, uint64_t{0});
  CHECK_EQ(timeline.buckets[1].sample_count, uint64_t{1});
  CHECK_EQ(timeline.buckets[0].span_starts, uint64_t{2});
  CHECK_EQ(timeline.buckets[1].alloc_bytes, uint64_t{50});
  CHECK_EQ(timeline.buckets[1].free_count, uint64_t{1});
  CHECK_NEAR(timeline.buckets[0].span_ns, 40.0, 0.5);
  CHECK_NEAR(timeline.buckets[1].span_ns, 50.0, 0.5);
  CHECK_NEAR(timeline.buckets[2].span_ns, 25.0, 0.5);
  CHECK_NEAR(timeline.buckets[3].span_ns, 25.0, 0.5);
  CHECK_EQ(timeline.total_alloc_bytes, uint64_t{150});
  CHECK_EQ(timeline.total_logs, uint64_t{1});
  CHECK_EQ(timeline.total_errors, uint64_t{1});
  CHECK_EQ(timeline.total_samples, uint64_t{1});
  CHECK_EQ(timeline.max_alloc_bytes, uint64_t{100});
  CHECK_NEAR(timeline.max_span_ns, 50.0, 0.5);
}

TEST(timeline_window_clips_events) {
  gbx::test::AnalysisFixture f;
  f.alloc(10, 0x1000, 100, "a.cpp:1", "Mesh");
  f.alloc(60, 0x2000, 200, "a.cpp:2", "Mesh");
  f.log(70, gbx::LogLevel::Info, "late");
  f.finalize();
  gbx::TimeWindow window;
  window.begin = 50;
  window.end = 100;
  const auto timeline = gbx::build_timeline(f.trace, 2, window);
  CHECK_EQ(timeline.buckets.size(), size_t{2});
  CHECK_EQ(timeline.total_alloc_bytes, uint64_t{200});
  CHECK_EQ(timeline.total_logs, uint64_t{1});
  CHECK_EQ(timeline.buckets[0].alloc_bytes, uint64_t{200});
  CHECK_EQ(timeline.buckets[1].alloc_bytes, uint64_t{0});
}

TEST(timeline_handles_empty_trace) {
  gbx::Trace trace;
  trace.finalize();
  const auto timeline = gbx::build_timeline(trace, 3);
  CHECK_EQ(timeline.buckets.size(), size_t{3});
  CHECK_EQ(timeline.total_alloc_bytes, uint64_t{0});
  CHECK_EQ(timeline.max_span_ns, 0.0);
  CHECK_EQ(timeline.buckets[0].end_ts, uint64_t{1});
}

TEST(timeline_bucket_count_is_clamped) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 1, 1, "a", "b");
  f.finalize();
  CHECK_EQ(gbx::build_timeline(f.trace, 0).buckets.size(), size_t{1});
  CHECK_EQ(gbx::build_timeline(f.trace, 9999).buckets.size(), size_t{512});
}
