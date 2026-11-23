// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Property-style tests over the analysis pipeline: totals must agree across
// independent reconstructions and repeated calls must be identical.
#include "glassbox/analysis.hpp"

#include <string>
#include <vector>

#include "glassbox/query.hpp"
#include "analysis_fixture.hpp"
#include "test_framework.hpp"

namespace {

uint64_t flame_self_total(const gbx::FlameNode& node) {
  uint64_t total = node.self;
  for (const auto& child : node.children) total += flame_self_total(child);
  return total;
}

size_t flame_nodes(const gbx::FlameNode& node) {
  size_t total = 1;
  for (const auto& child : node.children) total += flame_nodes(child);
  return total;
}

bool same_heap(const gbx::HeapSiteDelta& a, const gbx::HeapSiteDelta& b) {
  return a.site == b.site && a.klass == b.klass && a.bytes_a == b.bytes_a &&
         a.bytes_b == b.bytes_b && a.count_a == b.count_a &&
         a.count_b == b.count_b && a.bytes_delta == b.bytes_delta &&
         a.count_delta == b.count_delta;
}

}  // namespace

TEST(property_heap_totals_match_live_allocations) {
  gbx::test::AnalysisFixture f;
  uint64_t expected = 0;
  for (int i = 0; i < 40; ++i) {
    const uint64_t ptr = 0x1000 + static_cast<uint64_t>(i) * 0x20;
    const uint64_t size = 16 + static_cast<uint64_t>(i) * 7;
    f.alloc(static_cast<uint64_t>(i), ptr, size, "site.cpp:1", "Thing");
    expected += size;
    if (i % 3 == 0) {
      f.free(static_cast<uint64_t>(i) + 1, ptr);
      expected -= size;
    }
  }
  f.finalize();
  const auto heap = gbx::heap_at(f.trace, f.trace.end_ts());
  const auto live = gbx::live_allocations(f.trace, f.trace.end_ts(), 1000);
  uint64_t sum = 0;
  for (const auto& allocation : live) sum += allocation.size;
  CHECK_EQ(heap.total_bytes, sum);
  CHECK_EQ(heap.total_bytes, expected);
  CHECK_EQ(heap.object_count, live.size());

  uint64_t site_sum = 0;
  for (const auto& site : heap.sites) site_sum += site.live_bytes;
  CHECK_EQ(site_sum, heap.total_bytes);
}

TEST(property_timeline_totals_match_records) {
  gbx::test::AnalysisFixture f;
  uint64_t alloc_bytes = 0;
  uint64_t logs = 0;
  uint64_t errors = 0;
  for (int i = 0; i < 30; ++i) {
    f.alloc(static_cast<uint64_t>(i * 2), 0x100 + static_cast<uint64_t>(i), 10,
            "t.cpp:1", "T");
    alloc_bytes += 10;
    const auto level = i % 6 == 0 ? gbx::LogLevel::Error : gbx::LogLevel::Info;
    f.log(static_cast<uint64_t>(i * 2 + 1), level, "message");
    ++logs;
    if (level == gbx::LogLevel::Error) ++errors;
  }
  f.finalize();
  const auto timeline = gbx::build_timeline(f.trace, 17);
  CHECK_EQ(timeline.total_alloc_bytes, alloc_bytes);
  CHECK_EQ(timeline.total_logs, logs);
  CHECK_EQ(timeline.total_errors, errors);
  uint64_t sum = 0;
  for (const auto& bucket : timeline.buckets) sum += bucket.alloc_bytes;
  CHECK_EQ(sum, alloc_bytes);
}

TEST(property_flame_self_sums_to_total) {
  gbx::test::AnalysisFixture f;
  f.sample(1, {"all", "main", "Update"}, 3.0);
  f.sample(2, {"all", "main", "Render"});
  f.sample(3, {"all", "other"});
  f.finalize();
  const auto flame = gbx::build_flame(f.trace, {});
  CHECK_EQ(flame_self_total(flame.root), flame.total_weight);
  CHECK_EQ(flame.total_weight, uint64_t{5});
  CHECK_GT(flame_nodes(flame.root), size_t{3});
}

TEST(property_diff_is_repeatable) {
  gbx::test::AnalysisFixture f;
  f.alloc(5, 0x100, 64, "a.cpp:1", "A");
  f.alloc(10, 0x200, 128, "b.cpp:2", "B");
  f.free(15, 0x100);
  f.log(5, gbx::LogLevel::Info, "one");
  f.log(12, gbx::LogLevel::Error, "two");
  f.counter(5, "fps", 60);
  f.counter(12, "fps", 55);
  f.snapshot_number(5, "health", 100);
  f.snapshot_number(12, "health", 90);
  f.finalize();
  const auto first = gbx::diff(f.trace, 8, 20);
  const auto second = gbx::diff(f.trace, 8, 20);
  CHECK_EQ(first.heap.size(), second.heap.size());
  CHECK_EQ(first.spans.size(), second.spans.size());
  CHECK_EQ(first.counters.size(), second.counters.size());
  CHECK_EQ(first.logs.size(), second.logs.size());
  CHECK_EQ(first.state.size(), second.state.size());
  for (size_t i = 0; i < first.heap.size(); ++i) {
    CHECK(same_heap(first.heap[i], second.heap[i]));
  }
  for (size_t i = 0; i < first.state.size(); ++i) {
    CHECK_EQ(first.state[i].key, second.state[i].key);
    CHECK_EQ(first.state[i].before, second.state[i].before);
    CHECK_EQ(first.state[i].after, second.state[i].after);
  }
  const auto reverse = gbx::diff(f.trace, 20, 8);
  CHECK_EQ(reverse.total_bytes_delta, -first.total_bytes_delta);
  CHECK_EQ(reverse.total_bytes_a, first.total_bytes_b);
  CHECK_EQ(reverse.total_bytes_b, first.total_bytes_a);
}

TEST(property_query_normalization_and_repeatability) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0x10, 128, "x.cpp:1", "Mesh");
  f.log(2, gbx::LogLevel::Error, "failed to load mesh");
  f.finalize();
  gbx::QueryError error;
  const auto a = gbx::Query::parse("type=alloc AND size>=100", &error);
  const auto b = gbx::Query::parse("  type : alloc   AND   size >= 100 ", &error);
  CHECK(a.valid());
  CHECK(b.valid());
  CHECK_EQ(a.describe(), b.describe());
  const auto first = gbx::run_query(f.trace, a, 0, 100);
  const auto second = gbx::run_query(f.trace, a, 0, 100);
  CHECK_EQ(first.size(), second.size());
  CHECK_EQ(first.size(), size_t{1});
  CHECK_EQ(first[0], second[0]);
  const auto text = gbx::Query::parse("MESH", &error);
  CHECK(text.valid());
  CHECK_EQ(gbx::run_query(f.trace, text, 0, 100).size(), size_t{2});
}

TEST(property_state_history_is_monotonic_and_stable) {
  gbx::test::AnalysisFixture f;
  for (int i = 0; i < 50; ++i) {
    f.snapshot_number(static_cast<uint64_t>(i * 2), "value",
                      static_cast<double>(i));
  }
  f.finalize();
  const auto first = gbx::state_history(f.trace, "value", 20);
  const auto second = gbx::state_history(f.trace, "value", 20);
  CHECK_EQ(first.size(), second.size());
  for (size_t i = 1; i < first.size(); ++i) {
    CHECK(first[i - 1].ts <= first[i].ts);
  }
  for (size_t i = 0; i < first.size(); ++i) {
    CHECK_EQ(first[i].ts, second[i].ts);
    CHECK_NEAR(first[i].number, second[i].number, 1e-9);
  }
}

TEST(property_heap_graph_retained_covers_total) {
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    gbx::test::AnalysisFixture f;
    uint32_t state = seed * 2654435761u;
    auto next = [&state] {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      return state;
    };
    const uint64_t base = 0x100000 + static_cast<uint64_t>(seed) * 0x10000;
    uint64_t total = 0;
    for (int i = 0; i < 40; ++i) {
      const uint64_t ptr = base + static_cast<uint64_t>(i) * 64;
      const uint64_t size = 8 + (next() % 128);
      // Random owners produce both trees and cycles; self-owners are skipped.
      uint64_t owner = 0;
      if (i > 0 && (next() % 4) != 0) {
        owner = base + static_cast<uint64_t>(next() % static_cast<uint32_t>(i)) * 64;
      }
      f.alloc(static_cast<uint64_t>(i), ptr, size, "graph.cpp:1", "Node", owner);
      const bool self_owner = owner == ptr;
      if (!self_owner) total += size;
    }
    f.finalize();
    const auto graph = gbx::build_heap_graph(f.trace, f.trace.end_ts());
    CHECK(graph.converged);
    CHECK(!graph.nodes.empty());
    uint64_t retained_total = 0;
    for (const auto& node : graph.nodes) {
      CHECK(node.retained >= node.size);
      if (node.idom < 0) retained_total += node.retained;
    }
    CHECK_EQ(retained_total, graph.total_bytes);
    CHECK_EQ(graph.total_bytes, total);
    const size_t ordinals = gbx::live_allocations(f.trace, f.trace.end_ts()).size();
    CHECK_EQ(graph.nodes.size(), ordinals);
  }
}
