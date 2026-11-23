// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Verifies the committed golden demo trace. This protects the file format and
// the analysis pipeline against regressions that the generated-demo test might
// miss because it records a fresh file each run.
#include <string>

#include "glassbox/analysis.hpp"
#include "glassbox/trace_io.hpp"
#include "test_framework.hpp"

TEST(golden_demo_trace_loads_and_analyses) {
  const std::string path = std::string(GBX_SOURCE_DIR) + "/examples/traces/demo.gbt";
  gbx::Trace trace;
  const auto status = gbx::load_trace(path, trace);
  CHECK_MSG(status.ok(), status.describe());
  if (!status.ok()) return;
  CHECK(!trace.diagnostics.truncated);
  CHECK_GT(trace.size(), size_t{1000});
  CHECK_GT(trace.end_ts(), uint64_t{100'000'000});  // the demo has a 120 ms spike
  CHECK(!trace.chunks.empty());

  const auto spans = gbx::analyze_spans(trace);
  CHECK_GT(spans.spans.size(), size_t{400});
  CHECK_EQ(spans.unmatched_ends, uint64_t{0});
  CHECK_EQ(spans.unclosed, uint64_t{0});
  CHECK_GT(spans.roots.size(), size_t{0});

  const auto leaks = gbx::leak_candidates(trace, 20);
  CHECK(!leaks.empty());
  bool texture_leak = false;
  for (const auto& leak : leaks) {
    if (leak.site == "asset_loader.cpp:88") {
      texture_leak = true;
      CHECK_EQ(leak.bytes, uint64_t{3 * 32768});
    }
  }
  CHECK(texture_leak);

  const auto heap = gbx::heap_at(trace, trace.end_ts());
  CHECK_GT(heap.total_bytes, uint64_t{100'000});
  CHECK_GT(heap.peak_bytes, heap.total_bytes);
  CHECK_EQ(heap.unknown_frees, uint64_t{0});

  const auto graph = gbx::build_heap_graph(trace, trace.end_ts());
  CHECK(graph.converged);
  bool scene_root = false;
  for (const auto& node : graph.nodes) {
    if (node.site == "scene.cpp:41") {
      scene_root = true;
      CHECK_GT(node.retained, node.size);
      CHECK_EQ(node.retained, graph.total_bytes);
    }
  }
  CHECK(scene_root);

  const auto flame = gbx::build_flame(trace, {});
  CHECK(!flame.derived_from_spans);
  CHECK_GT(flame.total_weight, uint64_t{50});
  CHECK_EQ(flame.root.name, std::string("all"));

  const auto delta = gbx::diff(trace, trace.end_ts() / 4, trace.end_ts());
  CHECK_GT(delta.total_bytes_delta, int64_t{0});
  bool inventory_changed = false;
  for (const auto& entry : delta.state) {
    if (entry.key == "inventory_count") inventory_changed = entry.changed;
  }
  CHECK(inventory_changed);

  auto inventory = -1.0;
  for (const auto& entry : gbx::state_at(trace, trace.end_ts())) {
    if (entry.key == "inventory_count") inventory = entry.number;
  }
  CHECK_GT(inventory, 3.0);
}
