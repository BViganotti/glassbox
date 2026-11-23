// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>
#include <vector>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

TEST(span_analysis_tree) {
  gbx::test::AnalysisFixture f;
  const uint32_t a = f.begin_span("Root", 0);
  const uint32_t b = f.begin_span("Child", 10, a);
  f.end_span(b, 50, 1, a);
  const uint32_t c = f.begin_span("Sibling", 60, a);
  const uint32_t d = f.begin_span("Open", 70);
  f.end_span(c, 90, 1, a);
  f.end_span(999, 95);  // unmatched end
  f.end_span(a, 100);
  f.finalize();

  const auto analysis = gbx::analyze_spans(f.trace);
  CHECK_EQ(analysis.spans.size(), size_t{4});
  CHECK_EQ(analysis.roots.size(), size_t{2});
  CHECK_EQ(analysis.unmatched_ends, uint64_t{1});
  CHECK_EQ(analysis.unclosed, uint64_t{1});
  CHECK_EQ(analysis.by_id.size(), size_t{4});
  const auto root_index = analysis.by_id.at(a);
  const auto child_index = analysis.by_id.at(b);
  CHECK_EQ(analysis.spans[child_index].parent, a);
  CHECK_EQ(analysis.spans[child_index].depth, uint32_t{1});
  CHECK_EQ(analysis.spans[root_index].depth, uint32_t{0});
  CHECK_EQ(analysis.children[root_index].size(), size_t{2});
  CHECK_EQ(analysis.spans[analysis.by_id.at(d)].closed, false);
  CHECK_EQ(analysis.spans[analysis.by_id.at(c)].end_ts, uint64_t{90});
  CHECK_EQ(analysis.spans[analysis.by_id.at(d)].duration(f.trace.end_ts()),
           uint64_t{30});
}

TEST(span_analysis_duplicate_ids_are_ignored) {
  gbx::test::AnalysisFixture f;
  f.scratch.push_back({f.str("id"), gbx::Value::integer(7)});
  f.scratch.push_back({f.str("parent"), gbx::Value::integer(0)});
  f.scratch.push_back({f.str("name"), gbx::Value::str(f.str("A"))});
  f.emit(0, gbx::RecordType::SpanBegin);
  f.scratch.push_back({f.str("id"), gbx::Value::integer(7)});
  f.scratch.push_back({f.str("parent"), gbx::Value::integer(0)});
  f.scratch.push_back({f.str("name"), gbx::Value::str(f.str("B"))});
  f.emit(1, gbx::RecordType::SpanBegin);
  f.end_span(7, 2);
  f.end_span(7, 3);
  f.finalize();
  const auto analysis = gbx::analyze_spans(f.trace);
  CHECK_EQ(analysis.spans.size(), size_t{1});
  CHECK_EQ(analysis.spans[0].name, std::string("A"));
  CHECK_EQ(analysis.spans[0].end_ts, uint64_t{2});
  CHECK_EQ(analysis.unmatched_ends, uint64_t{1});
}

TEST(span_analysis_survives_parent_cycle) {
  gbx::test::AnalysisFixture f;
  const uint32_t a = f.begin_span("A", 0, 2);  // parent id 2 does not exist yet
  const uint32_t b = f.begin_span("B", 1, a);  // and now B exists as parent of A
  // Manually add an end for both; ids are 1 and 2.
  f.end_span(a, 5);
  f.end_span(b, 6);
  f.finalize();
  const auto analysis = gbx::analyze_spans(f.trace);
  CHECK_EQ(analysis.spans.size(), size_t{2});
  // A's parent id 2 is B, B's parent is A: the pair is a cycle, so neither is
  // a root, but depth computation is capped and analysis terminates.
  CHECK(analysis.spans[0].depth <= 64);
  CHECK(analysis.spans[1].depth <= 64);
  const auto active = gbx::active_spans(f.trace, 3);
  CHECK_EQ(active.size(), size_t{2});
}

TEST(active_spans_over_time) {
  gbx::test::AnalysisFixture f;
  const uint32_t a = f.begin_span("Root", 0);
  const uint32_t b = f.begin_span("Child", 10, a);
  f.end_span(b, 50, 1, a);
  const uint32_t d = f.begin_span("Open", 70);
  f.end_span(a, 100);
  f.finalize();

  auto at20 = gbx::active_spans(f.trace, 20);
  CHECK_EQ(at20.size(), size_t{2});
  CHECK_EQ(at20[0].name, std::string("Root"));
  CHECK_EQ(at20[0].depth, uint32_t{0});
  CHECK_EQ(at20[1].name, std::string("Child"));
  CHECK_EQ(at20[1].depth, uint32_t{1});
  CHECK_EQ(at20[1].parent, a);

  auto at60 = gbx::active_spans(f.trace, 60);
  CHECK_EQ(at60.size(), size_t{1});
  CHECK_EQ(at60[0].name, std::string("Root"));

  auto at80 = gbx::active_spans(f.trace, 80);
  CHECK_EQ(at80.size(), size_t{2});
  CHECK_EQ(at80[1].name, std::string("Open"));
  CHECK_EQ(at80[1].id, d);

  // The unclosed "Open" span stays active for the rest of the trace.
  auto at100 = gbx::active_spans(f.trace, 100);
  CHECK_EQ(at100.size(), size_t{1});
  CHECK_EQ(at100[0].name, std::string("Open"));
  auto at500 = gbx::active_spans(f.trace, 500);
  CHECK_EQ(at500.size(), size_t{1});
  CHECK_EQ(at500[0].name, std::string("Open"));
}
