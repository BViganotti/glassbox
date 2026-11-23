// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

namespace {

const gbx::HeapSiteDelta* find_heap_delta(const gbx::DiffResult& diff,
                                          std::string_view site) {
  for (const auto& delta : diff.heap) {
    if (delta.site == site) return &delta;
  }
  return nullptr;
}

}  // namespace

TEST(diff_detects_heap_growth_and_state_changes) {
  gbx::test::AnalysisFixture f;
  f.alloc(5, 0x1000, 100, "old.cpp:1", "Old");
  f.log(5, gbx::LogLevel::Info, "booting");
  f.counter(5, "fps", 60.0);
  f.snapshot_number(5, "health", 100);
  const uint32_t frame = f.begin_span("Frame", 5);
  f.alloc(15, 0x2000, 200, "leak.cpp:2", "Leak");
  f.free(20, 0x1000);
  f.log(15, gbx::LogLevel::Error, "leaking");
  f.counter(20, "fps", 50.0);
  f.snapshot_number(15, "health", 80);
  const uint32_t update = f.begin_span("Update", 15);
  f.end_span(update, 25);
  f.end_span(frame, 30);
  f.finalize();

  const auto result = gbx::diff(f.trace, 10, 20);
  CHECK_EQ(result.a, uint64_t{10});
  CHECK_EQ(result.b, uint64_t{20});
  CHECK_EQ(result.total_bytes_a, uint64_t{100});
  CHECK_EQ(result.total_bytes_b, uint64_t{200});
  CHECK_EQ(result.total_bytes_delta, int64_t{100});
  CHECK_EQ(result.objects_a, uint64_t{1});
  CHECK_EQ(result.objects_b, uint64_t{1});
  CHECK_EQ(result.objects_delta, int64_t{0});

  const auto* leak = find_heap_delta(result, "leak.cpp:2");
  CHECK(leak != nullptr);
  if (leak) {
    CHECK_EQ(leak->bytes_delta, int64_t{200});
    CHECK_EQ(leak->count_delta, int64_t{1});
  }
  const auto* old = find_heap_delta(result, "old.cpp:1");
  CHECK(old != nullptr);
  if (old) CHECK_EQ(old->bytes_delta, int64_t{-100});

  CHECK_EQ(result.spans.size(), size_t{1});
  CHECK_EQ(result.spans[0].name, std::string("Update"));
  CHECK_EQ(result.spans[0].count_delta, int64_t{1});
  CHECK_EQ(result.spans[0].total_ns_delta, int64_t{10});

  CHECK_EQ(result.logs.size(), size_t{1});
  CHECK(result.logs[0].level == gbx::LogLevel::Error);
  CHECK_EQ(result.logs[0].delta, int64_t{1});

  CHECK_EQ(result.counters.size(), size_t{1});
  CHECK_EQ(result.counters[0].name, std::string("fps"));
  CHECK_NEAR(result.counters[0].delta, -10.0, 1e-9);

  CHECK_EQ(result.state.size(), size_t{1});
  CHECK_EQ(result.state[0].key, std::string("health"));
  CHECK_EQ(result.state[0].before, std::string("100"));
  CHECK_EQ(result.state[0].after, std::string("80"));
  CHECK(result.state[0].changed);
}

TEST(diff_same_moment_is_empty) {
  gbx::test::AnalysisFixture f;
  f.alloc(5, 0x1000, 100, "a.cpp:1", "A");
  f.log(5, gbx::LogLevel::Info, "hello");
  f.counter(5, "fps", 60.0);
  f.finalize();
  const auto result = gbx::diff(f.trace, 10, 10);
  CHECK(result.heap.empty());
  CHECK(result.spans.empty());
  CHECK(result.logs.empty());
  CHECK(result.counters.empty());
  CHECK(result.state.empty());
  CHECK_EQ(result.total_bytes_delta, int64_t{0});
}

TEST(diff_respects_limit) {
  gbx::test::AnalysisFixture f;
  for (int i = 0; i < 10; ++i) {
    f.alloc(static_cast<uint64_t>(i + 1), static_cast<uint64_t>(0x100 + i), 10,
            "site" + std::to_string(i), "K");
  }
  f.finalize();
  const auto result = gbx::diff(f.trace, 0, 100, 3);
  CHECK_EQ(result.heap.size(), size_t{3});
}

TEST(diff_backwards_is_computable) {
  gbx::test::AnalysisFixture f;
  f.alloc(30, 0x1000, 300, "late.cpp:1", "Late");
  f.finalize();
  const auto result = gbx::diff(f.trace, 50, 10);
  CHECK_EQ(result.total_bytes_a, uint64_t{300});
  CHECK_EQ(result.total_bytes_b, uint64_t{0});
  CHECK_EQ(result.total_bytes_delta, int64_t{-300});
}
