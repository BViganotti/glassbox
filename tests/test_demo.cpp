// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/demo.hpp"

#include <unistd.h>

#include <filesystem>
#include <string>

#include "glassbox/analysis.hpp"
#include "trace/file_io.hpp"
#include "glassbox/trace_io.hpp"
#include "test_framework.hpp"

namespace {

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-demo-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  std::filesystem::remove(dir / name);
  return (dir / name).string();
}

size_t count_type(const gbx::Trace& trace, gbx::RecordType type) {
  size_t n = 0;
  for (const auto& record : trace.records) {
    if (record.type == type) ++n;
  }
  return n;
}

gbx::DemoOptions fast_options() {
  gbx::DemoOptions options;
  options.frames = 50;
  options.sleep_spike = false;
  options.use_mach_sampler = false;
  return options;
}

}  // namespace

TEST(demo_records_a_complete_story) {
  const std::string path = temp_path("story.gbt");
  const auto result = gbx::record_demo_trace(path, fast_options());
  CHECK_MSG(result.status.ok(), result.status.describe());
  CHECK_EQ(result.frames_recorded, uint64_t{50});
  CHECK_GT(result.allocations, uint64_t{0});
  CHECK_GT(result.leaks, uint64_t{0});
  CHECK_GT(result.samples, uint64_t{0});
  CHECK(gbx::trace::is_regular_file(path));

  gbx::Trace trace;
  auto status = gbx::load_trace(path, trace);
  CHECK_MSG(status.ok(), status.describe());
  CHECK_EQ(count_type(trace, gbx::RecordType::SpanBegin) >= size_t{50}, true);
  CHECK_GT(count_type(trace, gbx::RecordType::Sample), size_t{0});
  CHECK_GT(count_type(trace, gbx::RecordType::Alloc), size_t{0});
  CHECK_GT(count_type(trace, gbx::RecordType::Snapshot), size_t{0});

  bool saw_slow_frame = false;
  for (const auto& record : trace.records) {
    if (record.type == gbx::RecordType::SpanBegin &&
        trace.str_field(record, "name") == "SlowFrame") {
      saw_slow_frame = true;
    }
  }
  CHECK(saw_slow_frame);

  const auto leaks = gbx::leak_candidates(trace, 20);
  CHECK(!leaks.empty());
  bool saw_texture_leak = false;
  for (const auto& leak : leaks) {
    if (leak.site == "asset_loader.cpp:88") {
      saw_texture_leak = true;
      CHECK_EQ(leak.bytes, uint64_t{2 * 32768});
      CHECK_EQ(leak.count, uint64_t{2});
    }
  }
  CHECK(saw_texture_leak);

  const auto state = gbx::state_at(trace, trace.end_ts());
  double inventory = -1;
  double expected = -1;
  for (const auto& entry : state) {
    if (entry.key == "inventory_count") inventory = entry.number;
    if (entry.key == "inventory_expected") expected = entry.number;
  }
  CHECK_NEAR(expected, 3.0, 1e-9);
  CHECK_GT(inventory, expected);

  const auto flame = gbx::build_flame(trace, {});
  CHECK(!flame.derived_from_spans);
  CHECK_GT(flame.total_weight, uint64_t{0});
  CHECK(!flame.root.children.empty());

  const auto graph = gbx::build_heap_graph(trace, trace.end_ts());
  CHECK(graph.converged);
  CHECK_GT(graph.roots, size_t{0});
  bool saw_scene = false;
  for (const auto& node : graph.nodes) {
    if (node.site == "scene.cpp:41") {
      saw_scene = true;
      CHECK_GT(node.retained, node.size);
    }
  }
  CHECK(saw_scene);

  const auto delta = gbx::diff(trace, 0, trace.end_ts());
  bool saw_inventory_delta = false;
  for (const auto& entry : delta.state) {
    if (entry.key == "inventory_count") {
      saw_inventory_delta = true;
      CHECK(entry.changed);
    }
  }
  CHECK(saw_inventory_delta);
}

TEST(demo_is_structurally_deterministic) {
  const auto options = fast_options();
  const std::string first = temp_path("det-a.gbt");
  const std::string second = temp_path("det-b.gbt");
  CHECK(gbx::record_demo_trace(first, options).status.ok());
  CHECK(gbx::record_demo_trace(second, options).status.ok());
  gbx::Trace a;
  gbx::Trace b;
  CHECK(gbx::load_trace(first, a).ok());
  CHECK(gbx::load_trace(second, b).ok());
  CHECK_EQ(count_type(a, gbx::RecordType::SpanBegin),
           count_type(b, gbx::RecordType::SpanBegin));
  CHECK_EQ(count_type(a, gbx::RecordType::Alloc),
           count_type(b, gbx::RecordType::Alloc));
  CHECK_EQ(count_type(a, gbx::RecordType::Free),
           count_type(b, gbx::RecordType::Free));
  CHECK_EQ(count_type(a, gbx::RecordType::Sample),
           count_type(b, gbx::RecordType::Sample));
  CHECK_EQ(gbx::leak_candidates(a, 10).size(),
           gbx::leak_candidates(b, 10).size());
}

TEST(demo_requires_started_recorder) {
  gbx::Recorder recorder;
  gbx::DemoOptions options;
  gbx::DemoResult result;
  auto status = gbx::run_demo(recorder, options, result);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::InvalidArgument);
}
