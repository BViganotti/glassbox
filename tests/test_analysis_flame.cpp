// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>
#include <vector>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

namespace {

const gbx::FlameNode* find_child(const gbx::FlameNode& node,
                                 std::string_view name) {
  for (const auto& child : node.children) {
    if (child.name == name) return &child;
  }
  return nullptr;
}

}  // namespace

TEST(flame_aggregates_sample_stacks) {
  gbx::test::AnalysisFixture f;
  f.sample(10, {"main", "Update", "Physics"});
  f.sample(20, {"main", "Update", "Physics"}, 2.0);
  f.sample(30, {"main", "Render"});
  f.finalize();

  const auto flame = gbx::build_flame(f.trace, {});
  CHECK(!flame.derived_from_spans);
  CHECK_EQ(flame.samples, uint64_t{3});
  CHECK_EQ(flame.total_weight, uint64_t{4});
  CHECK_EQ(flame.root.name, std::string("all"));
  CHECK_EQ(flame.root.total, uint64_t{4});
  const auto* main = find_child(flame.root, "main");
  CHECK(main != nullptr);
  if (main) {
    CHECK_EQ(main->total, uint64_t{4});
    CHECK_EQ(main->samples, uint64_t{3});
    const auto* update = find_child(*main, "Update");
    const auto* render = find_child(*main, "Render");
    CHECK(update != nullptr);
    CHECK(render != nullptr);
    if (update) {
      CHECK_EQ(update->total, uint64_t{3});
      CHECK_EQ(update->self, uint64_t{0});
      CHECK_EQ(update->samples, uint64_t{2});
      const auto* physics = find_child(*update, "Physics");
      CHECK(physics != nullptr);
      if (physics) {
        CHECK_EQ(physics->total, uint64_t{3});
        CHECK_EQ(physics->self, uint64_t{3});
      }
    }
    if (render) {
      CHECK_EQ(render->total, uint64_t{1});
      CHECK_EQ(render->self, uint64_t{1});
    }
  }
}

TEST(flame_respects_time_window) {
  gbx::test::AnalysisFixture f;
  f.sample(10, {"main", "Early"});
  f.sample(30, {"main", "Late"});
  f.finalize();
  gbx::TimeWindow window;
  window.begin = 25;
  window.end = 100;
  const auto flame = gbx::build_flame(f.trace, window);
  CHECK_EQ(flame.total_weight, uint64_t{1});
  CHECK_EQ(flame.samples, uint64_t{1});
  const auto* main = find_child(flame.root, "main");
  CHECK(main != nullptr);
  if (main) {
    CHECK(find_child(*main, "Late") != nullptr);
    CHECK(find_child(*main, "Early") == nullptr);
  }
}

TEST(flame_truncates_node_budget) {
  gbx::test::AnalysisFixture f;
  f.sample(1, {"a", "b", "c"});
  f.sample(2, {"d"});
  f.finalize();
  gbx::FlameOptions options;
  options.max_nodes = 2;
  const auto flame = gbx::build_flame(f.trace, {}, options);
  CHECK(flame.truncated);
  CHECK(flame.root.children.size() <= size_t{1});
  CHECK_EQ(flame.root.total, uint64_t{2});
}

TEST(flame_depth_limit) {
  gbx::test::AnalysisFixture f;
  f.sample(1, {"a", "b", "c"});
  f.finalize();
  gbx::FlameOptions options;
  options.max_depth = 1;
  const auto flame = gbx::build_flame(f.trace, {}, options);
  const auto* a = find_child(flame.root, "a");
  CHECK(a != nullptr);
  if (a) CHECK_EQ(a->children.size(), size_t{0});
  CHECK(flame.truncated);
}

TEST(flame_falls_back_to_spans) {
  gbx::test::AnalysisFixture f;
  const uint32_t a = f.begin_span("Frame", 0);
  const uint32_t b = f.begin_span("Physics", 10, a);
  f.end_span(b, 50, 1, a);
  f.end_span(a, 100);
  f.finalize();
  const auto flame = gbx::build_flame(f.trace, {});
  CHECK(flame.derived_from_spans);
  CHECK_EQ(flame.total_weight, uint64_t{100});
  const auto* frame = find_child(flame.root, "Frame");
  CHECK(frame != nullptr);
  if (frame) {
    CHECK_EQ(frame->total, uint64_t{100});
    CHECK_EQ(frame->self, uint64_t{60});
    const auto* physics = find_child(*frame, "Physics");
    CHECK(physics != nullptr);
    if (physics) {
      CHECK_EQ(physics->total, uint64_t{40});
      CHECK_EQ(physics->self, uint64_t{40});
    }
  }
}

TEST(flame_empty_trace) {
  gbx::Trace trace;
  trace.finalize();
  const auto flame = gbx::build_flame(trace, {});
  CHECK_EQ(flame.total_weight, uint64_t{0});
  CHECK_EQ(flame.root.children.size(), size_t{0});
  CHECK(flame.derived_from_spans);
}
