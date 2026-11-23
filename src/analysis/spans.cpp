// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <vector>

namespace gbx {
namespace {

constexpr uint32_t kMaxDepth = 64;

uint32_t depth_of(const SpanAnalysis& analysis, size_t index) {
  uint32_t depth = 0;
  uint32_t guard = 0;
  uint32_t parent = analysis.spans[index].parent;
  while (parent != 0 && guard < kMaxDepth) {
    auto it = analysis.by_id.find(parent);
    if (it == analysis.by_id.end()) break;
    if (it->second == index) break;  // self-parent
    ++depth;
    ++guard;
    parent = analysis.spans[it->second].parent;
  }
  return depth;
}

}  // namespace

SpanAnalysis analyze_spans(const Trace& trace) {
  SpanAnalysis analysis;
  analysis.spans.reserve(std::min<size_t>(trace.records.size(), 100000));
  for (const auto& record : trace.records) {
    if (record.type == RecordType::SpanBegin) {
      const int64_t id = trace.int_field(record, keys::kId);
      if (id <= 0 || id > std::numeric_limits<uint32_t>::max()) continue;
      if (analysis.by_id.find(static_cast<uint32_t>(id)) !=
          analysis.by_id.end()) {
        continue;  // duplicate ids are ignored rather than patched
      }
      SpanInfo span;
      span.id = static_cast<uint32_t>(id);
      span.parent = static_cast<uint32_t>(
          std::max<int64_t>(0, trace.int_field(record, keys::kParent, 0)));
      span.thread = record.thread;
      span.begin_ts = record.ts;
      span.name = std::string(trace.str_field(record, keys::kName));
      span.kind = std::string(trace.str_field(record, keys::kKind));
      span.record_index = static_cast<uint32_t>(&record - trace.records.data());
      analysis.by_id.emplace(span.id, analysis.spans.size());
      analysis.spans.push_back(std::move(span));
    } else if (record.type == RecordType::SpanEnd) {
      const int64_t id = trace.int_field(record, keys::kId);
      if (id <= 0) {
        ++analysis.unmatched_ends;
        continue;
      }
      auto it = analysis.by_id.find(static_cast<uint32_t>(id));
      if (it == analysis.by_id.end()) {
        ++analysis.unmatched_ends;
        continue;
      }
      auto& span = analysis.spans[it->second];
      if (span.closed) {
        ++analysis.unmatched_ends;
        continue;
      }
      span.closed = true;
      span.end_ts = record.ts;
    }
  }

  analysis.children.resize(analysis.spans.size());
  for (size_t i = 0; i < analysis.spans.size(); ++i) {
    auto& span = analysis.spans[i];
    auto it = analysis.by_id.find(span.parent);
    if (span.parent != 0 && it != analysis.by_id.end() && it->second != i) {
      analysis.children[it->second].push_back(static_cast<uint32_t>(i));
    } else {
      span.parent = 0;
      analysis.roots.push_back(static_cast<uint32_t>(i));
    }
  }
  for (size_t i = 0; i < analysis.spans.size(); ++i) {
    analysis.spans[i].depth = depth_of(analysis, i);
    if (!analysis.spans[i].closed) ++analysis.unclosed;
    analysis.total_duration_ns +=
        analysis.spans[i].duration(trace.end_ts());
  }
  return analysis;
}

std::vector<ActiveSpan> active_spans(const Trace& trace, uint64_t at) {
  const uint64_t end = trace.end_ts();
  const SpanAnalysis analysis = analyze_spans(trace);
  std::vector<ActiveSpan> out;
  out.reserve(32);
  for (const auto& span : analysis.spans) {
    if (span.begin_ts > at) continue;
    if (span.closed && span.end_ts <= at) continue;
    ActiveSpan active;
    active.id = span.id;
    active.parent = span.parent;
    active.thread = span.thread;
    active.depth = span.depth;
    active.begin_ts = span.begin_ts;
    active.name = span.name;
    active.kind = span.kind;
    out.push_back(std::move(active));
  }
  std::sort(out.begin(), out.end(), [](const ActiveSpan& a,
                                       const ActiveSpan& b) {
    if (a.begin_ts != b.begin_ts) return a.begin_ts < b.begin_ts;
    return a.id < b.id;
  });
  (void)end;
  return out;
}

}  // namespace gbx
