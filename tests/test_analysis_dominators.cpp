// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

namespace {

const gbx::HeapGraphNode* find_node(const gbx::HeapGraph& graph,
                                    uint64_t ptr) {
  for (const auto& node : graph.nodes) {
    if (node.ptr == ptr) return &node;
  }
  return nullptr;
}

}  // namespace

TEST(heap_graph_retained_sizes_chain) {
  gbx::test::AnalysisFixture f;
  // A owns B and C; B owns D.
  f.alloc(1, 0x10, 100, "a.cpp:1", "A");
  f.alloc(2, 0x20, 50, "b.cpp:2", "B", 0x10);
  f.alloc(3, 0x30, 30, "c.cpp:3", "C", 0x10);
  f.alloc(4, 0x40, 20, "d.cpp:4", "D", 0x20);
  f.finalize();

  const auto graph = gbx::build_heap_graph(f.trace, 10);
  CHECK(graph.converged);
  CHECK(!graph.truncated);
  CHECK_EQ(graph.nodes.size(), size_t{4});
  CHECK_EQ(graph.roots, size_t{1});
  CHECK_EQ(graph.total_bytes, uint64_t{200});
  const auto* a = find_node(graph, 0x10);
  const auto* b = find_node(graph, 0x20);
  const auto* c = find_node(graph, 0x30);
  const auto* d = find_node(graph, 0x40);
  CHECK(a && b && c && d);
  if (a && b && c && d) {
    CHECK_EQ(a->retained, uint64_t{200});
    CHECK_EQ(b->retained, uint64_t{70});
    CHECK_EQ(c->retained, uint64_t{30});
    CHECK_EQ(d->retained, uint64_t{20});
    CHECK_EQ(a->idom, -1);
    CHECK_EQ(b->idom, 0);
    CHECK_EQ(c->idom, 0);
    CHECK_EQ(d->idom, 1);
    CHECK_EQ(a->children.size(), size_t{2});
    CHECK_EQ(b->children.size(), size_t{1});
    CHECK_EQ(d->children.size(), size_t{0});
  }
}

TEST(heap_graph_multiple_roots) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0x10, 10, "a", "A");
  f.alloc(2, 0x20, 20, "b", "B");
  f.alloc(3, 0x30, 30, "c", "C", 0x20);
  f.finalize();
  const auto graph = gbx::build_heap_graph(f.trace, 10);
  CHECK_EQ(graph.roots, size_t{2});
  const auto* b = find_node(graph, 0x20);
  const auto* c = find_node(graph, 0x30);
  CHECK(b && c);
  if (b && c) {
    CHECK_EQ(b->retained, uint64_t{50});
    CHECK_EQ(c->retained, uint64_t{30});
  }
}

TEST(heap_graph_owner_cycle_is_safe) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0x100, 100, "a", "A", 0x200);
  f.alloc(2, 0x200, 50, "b", "B", 0x100);
  f.alloc(3, 0x300, 10, "self", "Self", 0x300);
  f.finalize();
  const auto graph = gbx::build_heap_graph(f.trace, 10);
  CHECK(graph.converged);
  CHECK_EQ(graph.nodes.size(), size_t{3});
  CHECK_GE(graph.roots, size_t{1});
  uint64_t root_retained = 0;
  for (const auto& node : graph.nodes) {
    if (node.idom < 0) root_retained += node.retained;
  }
  CHECK_EQ(root_retained, graph.total_bytes);
}

TEST(heap_graph_truncates_nodes) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0x10, 100, "a", "A");
  f.alloc(2, 0x20, 200, "b", "B");
  f.alloc(3, 0x30, 300, "c", "C");
  f.finalize();
  gbx::HeapGraphOptions options;
  options.max_nodes = 2;
  const auto graph = gbx::build_heap_graph(f.trace, 10, options);
  CHECK(graph.truncated);
  CHECK_EQ(graph.nodes.size(), size_t{2});
  CHECK_EQ(graph.total_bytes, uint64_t{500});
}

TEST(heap_graph_truncates_edges) {
  gbx::test::AnalysisFixture f;
  f.alloc(1, 0x10, 10, "a", "A");
  f.alloc(2, 0x20, 20, "b", "B", 0x10);
  f.alloc(3, 0x30, 30, "c", "C", 0x20);
  f.finalize();
  gbx::HeapGraphOptions options;
  options.max_edges = 1;
  const auto graph = gbx::build_heap_graph(f.trace, 10, options);
  CHECK(graph.truncated);
  CHECK_EQ(graph.nodes.size(), size_t{3});
  CHECK(graph.converged);
}

TEST(heap_graph_empty) {
  gbx::test::AnalysisFixture f;
  f.marker(1, "nothing");
  f.finalize();
  const auto graph = gbx::build_heap_graph(f.trace, 10);
  CHECK(graph.nodes.empty());
  CHECK(graph.converged);
  CHECK_EQ(graph.total_bytes, uint64_t{0});
  CHECK_EQ(graph.roots, size_t{0});
}
