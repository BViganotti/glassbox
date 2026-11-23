// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "glassbox/analysis.hpp"
#include "glassbox/sample.hpp"

namespace gbx {
namespace {

struct FlatNode {
  std::string name;
  uint64_t self = 0;
  uint64_t total = 0;
  uint64_t samples = 0;
  std::vector<uint32_t> children;
};

struct FlameBuilder {
  std::vector<FlatNode> nodes;
  size_t max_nodes = 50000;
  size_t max_depth = 128;
  bool truncated = false;

  FlameBuilder() {
    nodes.emplace_back();
    nodes[0].name = "all";
  }

  uint32_t child_of(uint32_t parent, std::string_view name) {
    for (uint32_t index : nodes[parent].children) {
      if (nodes[index].name == name) return index;
    }
    if (nodes.size() >= max_nodes) {
      truncated = true;
      return UINT32_MAX;
    }
    FlatNode created;
    created.name = std::string(name);
    nodes.push_back(std::move(created));
    nodes[parent].children.push_back(static_cast<uint32_t>(nodes.size() - 1));
    return static_cast<uint32_t>(nodes.size() - 1);
  }

  void add_stack(const std::vector<std::string>& frames, uint64_t weight,
                 size_t depth_limit) {
    nodes[0].total += weight;
    nodes[0].samples += 1;
    uint32_t current = 0;
    size_t depth = 0;
    for (const auto& frame : frames) {
      if (depth >= depth_limit) {
        truncated = true;
        break;
      }
      const uint32_t next = child_of(current, frame);
      if (next == UINT32_MAX) break;
      nodes[next].total += weight;
      nodes[next].samples += 1;
      current = next;
      ++depth;
    }
    nodes[current].self += weight;
  }
};

FlameNode convert(const std::vector<FlatNode>& flat, uint32_t index,
                  size_t depth, size_t& budget, size_t max_depth,
                  uint64_t& dropped) {
  FlameNode node;
  node.name = flat[index].name;
  node.self = flat[index].self;
  node.total = flat[index].total;
  node.samples = flat[index].samples;
  if (depth >= max_depth) {
    if (!flat[index].children.empty()) dropped += flat[index].children.size();
    return node;
  }
  std::vector<uint32_t> children = flat[index].children;
  std::sort(children.begin(), children.end(),
            [&flat](uint32_t a, uint32_t b) {
              if (flat[a].total != flat[b].total) {
                return flat[a].total > flat[b].total;
              }
              return flat[a].name < flat[b].name;
            });
  for (uint32_t child : children) {
    if (budget == 0) {
      ++dropped;
      continue;
    }
    --budget;
    node.children.push_back(
        convert(flat, child, depth + 1, budget, max_depth, dropped));
  }
  return node;
}

FlameGraph finish(const FlameBuilder& builder, TimeWindow window,
                  bool from_spans, size_t max_depth, size_t max_nodes) {
  FlameGraph graph;
  graph.window = window;
  graph.derived_from_spans = from_spans;
  graph.truncated = builder.truncated;
  graph.total_weight = builder.nodes[0].total;
  graph.samples = builder.nodes[0].samples;
  size_t budget = max_depth == 0 ? 0 : max_nodes;
  uint64_t dropped = 0;
  graph.root = convert(builder.nodes, 0, 0, budget, max_depth, dropped);
  graph.truncated_nodes += dropped;
  if (dropped > 0) graph.truncated = true;
  return graph;
}

std::vector<std::string> split_frames(std::string_view text, size_t max_frame) {
  std::vector<std::string> frames;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t sep = text.find(kFrameSeparator, start);
    const size_t end = sep == std::string_view::npos ? text.size() : sep;
    std::string_view frame = text.substr(start, end - start);
    if (frame.size() > max_frame) frame = frame.substr(0, max_frame);
    frames.emplace_back(frame.empty() ? "<unknown>" : std::string(frame));
    if (sep == std::string_view::npos) break;
    start = sep + 1;
  }
  return frames;
}

}  // namespace

FlameGraph build_flame(const Trace& trace, TimeWindow window,
                       const FlameOptions& options) {
  if (window.end == 0) window.end = trace.end_ts();
  if (window.end < window.begin) window.end = window.begin;
  FlameBuilder builder;
  builder.max_nodes = std::max<size_t>(options.max_nodes, 2);
  builder.max_depth = std::max<size_t>(options.max_depth, 1);
  const auto [first, last] = trace.record_range(window.begin, window.end);
  for (size_t i = first; i < last; ++i) {
    const auto& record = trace.records[i];
    if (record.type != RecordType::Sample) continue;
    const auto text = trace.str_field(record, keys::kFrames);
    if (text.empty()) continue;
    double weight = trace.double_field(record, keys::kWeight, 1.0);
    if (!(weight > 0.0) || weight > 1e12) weight = 1.0;
    const auto frames = split_frames(text, options.max_frame_length);
    builder.add_stack(frames, static_cast<uint64_t>(weight),
                      options.max_depth);
  }
  if (builder.nodes[0].total == 0) {
    FlameGraph span_graph = build_span_flame(trace, window, options);
    return span_graph;
  }
  return finish(builder, window, false, options.max_depth, builder.max_nodes);
}

FlameGraph build_span_flame(const Trace& trace, TimeWindow window,
                            const FlameOptions& options) {
  if (window.end == 0) window.end = trace.end_ts();
  if (window.end < window.begin) window.end = window.begin;
  FlameBuilder builder;
  builder.max_nodes = std::max<size_t>(options.max_nodes, 2);
  builder.max_depth = std::max<size_t>(options.max_depth, 1);
  const SpanAnalysis analysis = analyze_spans(trace);

  std::vector<uint64_t> clipped(analysis.spans.size(), 0);
  std::vector<uint64_t> child_sum(analysis.spans.size(), 0);
  for (size_t i = 0; i < analysis.spans.size(); ++i) {
    const auto& span = analysis.spans[i];
    const uint64_t end = span.closed ? span.end_ts : trace.end_ts();
    const uint64_t lo = std::max(span.begin_ts, window.begin);
    const uint64_t hi = std::min(end, window.end);
    clipped[i] = hi > lo ? hi - lo : 0;
  }
  for (size_t i = 0; i < analysis.spans.size(); ++i) {
    for (uint32_t child : analysis.children[i]) {
      child_sum[i] += clipped[child];
    }
  }

  for (size_t i = 0; i < analysis.spans.size(); ++i) {
    if (clipped[i] == 0) continue;
    const uint64_t exclusive =
        clipped[i] > child_sum[i] ? clipped[i] - child_sum[i] : 0;
    // Build the root-first path with a hard cap.
    std::vector<std::string> frames;
    uint32_t current = static_cast<uint32_t>(i);
    uint32_t guard = 0;
    while (guard < 64) {
      const auto& span = analysis.spans[current];
      frames.push_back(span.name.empty() ? "<unnamed>" : span.name);
      if (span.parent == 0) break;
      auto it = analysis.by_id.find(span.parent);
      if (it == analysis.by_id.end() || it->second == current) break;
      current = static_cast<uint32_t>(it->second);
      ++guard;
    }
    std::reverse(frames.begin(), frames.end());
    builder.add_stack(frames, exclusive, options.max_depth);
  }
  return finish(builder, window, true, options.max_depth, builder.max_nodes);
}

}  // namespace gbx
