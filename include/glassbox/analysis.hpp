// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "glassbox/trace.hpp"

namespace gbx {

struct TimeWindow {
  uint64_t begin = 0;
  uint64_t end = 0;  // inclusive; 0 means "end of trace" for analysis helpers
};

// --- spans -----------------------------------------------------------------

struct SpanInfo {
  uint32_t id = 0;
  uint32_t parent = 0;
  uint32_t thread = 0;
  uint64_t begin_ts = 0;
  uint64_t end_ts = 0;
  bool closed = false;
  std::string name;
  std::string kind;
  uint32_t record_index = 0;
  uint32_t depth = 0;

  uint64_t duration(uint64_t fallback_end) const {
    return (closed ? end_ts : fallback_end) > begin_ts
               ? (closed ? end_ts : fallback_end) - begin_ts
               : 0;
  }
};

/// Span tree reconstructed from SpanBegin/SpanEnd records. Ends without a
/// matching begin are counted, not fabricated. Unclosed spans stay open until
/// the end of the trace.
struct SpanAnalysis {
  std::vector<SpanInfo> spans;
  std::unordered_map<uint32_t, size_t> by_id;
  std::vector<uint32_t> roots;                       // span indices
  std::vector<std::vector<uint32_t>> children;       // aligned with spans
  uint64_t unmatched_ends = 0;
  uint64_t unclosed = 0;
  uint64_t total_duration_ns = 0;
};

SpanAnalysis analyze_spans(const Trace& trace);

struct ActiveSpan {
  uint32_t id = 0;
  uint32_t parent = 0;
  uint32_t thread = 0;
  uint32_t depth = 0;
  uint64_t begin_ts = 0;
  std::string name;
  std::string kind;
};

/// Spans active at `at`, ordered by begin time then id. Depths are derived
/// from the parent chain with a hard cap to survive malformed parent cycles.
std::vector<ActiveSpan> active_spans(const Trace& trace, uint64_t at);

// --- timeline --------------------------------------------------------------

struct TimelineBucket {
  uint64_t begin_ts = 0;
  uint64_t end_ts = 0;
  uint64_t alloc_bytes = 0;
  uint64_t alloc_count = 0;
  uint64_t free_count = 0;
  uint64_t log_count = 0;
  uint64_t error_count = 0;
  uint64_t sample_count = 0;
  uint64_t span_starts = 0;
  double span_ns = 0.0;  // summed span durations overlapping the bucket
};

struct Timeline {
  std::vector<TimelineBucket> buckets;
  uint64_t begin_ts = 0;
  uint64_t end_ts = 0;
  uint64_t bucket_width = 1;
  double max_span_ns = 0.0;
  uint64_t max_alloc_bytes = 0;
  uint64_t total_alloc_bytes = 0;
  uint64_t total_logs = 0;
  uint64_t total_errors = 0;
  uint64_t total_samples = 0;
};

Timeline build_timeline(const Trace& trace, uint32_t bucket_count,
                        TimeWindow window = {});

// --- counters and state ----------------------------------------------------

struct SeriesPoint {
  uint64_t ts = 0;
  double value = 0;
};

struct Series {
  std::string name;
  std::string unit;
  RecordType kind = RecordType::Counter;
  std::vector<SeriesPoint> points;
  double minimum = 0;
  double maximum = 0;
  double last = 0;
};

/// Counter/gauge series (name filter is a case-insensitive substring; empty
/// means all). Points are evenly downsampled when they exceed max_points.
std::vector<Series> counter_series(const Trace& trace,
                                   std::string_view name_filter = {},
                                   size_t max_series = 32,
                                   size_t max_points = 2048);

/// Last value of a series at or before `at`.
bool series_value_at(const Trace& trace, std::string_view name, uint64_t at,
                     double& value);

struct StateEntry {
  std::string key;
  ValueType type = ValueType::Float;
  double number = 0;
  std::string text;
  uint64_t ts = 0;
};

/// Merged state of all Snapshot records up to `at` (last write wins), sorted
/// by key. Numeric and string values are both represented.
std::vector<StateEntry> state_at(const Trace& trace, uint64_t at);

// --- heap ------------------------------------------------------------------

struct HeapAllocation {
  uint64_t ptr = 0;
  uint64_t size = 0;
  uint64_t owner = 0;
  uint64_t alloc_ts = 0;
  uint64_t free_ts = 0;
  bool freed = false;
  std::string site;
  std::string klass;
  uint32_t alloc_record = 0;
};

struct HeapSiteStat {
  std::string site;
  std::string klass;
  uint64_t live_bytes = 0;
  uint64_t live_count = 0;
  uint64_t alloc_bytes = 0;
  uint64_t alloc_count = 0;
  uint64_t free_count = 0;
  double average_age_ns = 0;
};

struct HeapObjectView {
  uint64_t ptr = 0;
  uint64_t size = 0;
  uint64_t owner = 0;
  uint64_t alloc_ts = 0;
  uint64_t age_ns = 0;
  std::string site;
  std::string klass;
};

struct HeapAt {
  uint64_t at = 0;
  uint64_t total_bytes = 0;
  uint64_t object_count = 0;
  uint64_t alloc_count = 0;
  uint64_t free_count = 0;
  uint64_t unknown_frees = 0;
  uint64_t peak_bytes = 0;
  uint64_t peak_ts = 0;
  std::vector<HeapSiteStat> sites;
  std::vector<HeapObjectView> objects;
  size_t objects_truncated = 0;
  size_t sites_truncated = 0;
};

struct HeapOptions {
  size_t max_objects = 5000;
  size_t max_sites = 256;
};

/// Deterministic heap reconstruction. Allocations are matched by pointer;
/// reallocating the same pointer implicitly ends the previous allocation.
HeapAt heap_at(const Trace& trace, uint64_t at, const HeapOptions& options = {});

/// Live allocations at `at`, largest first, capped at `limit`.
std::vector<HeapAllocation> live_allocations(const Trace& trace, uint64_t at,
                                             size_t limit = 10000);

struct LeakCandidate {
  std::string site;
  std::string klass;
  uint64_t bytes = 0;
  uint64_t count = 0;
  uint64_t oldest_ts = 0;
  uint64_t newest_ts = 0;
  double score = 0;
};

/// Groups allocations that are still live at the end of the trace. Score
/// weights bytes by how long the memory has been alive; ties break by bytes
/// and then site/class, so results are deterministic.
std::vector<LeakCandidate> leak_candidates(const Trace& trace,
                                           size_t limit = 50);

// --- flame graphs ----------------------------------------------------------

struct FlameNode {
  std::string name;
  uint64_t self = 0;
  uint64_t total = 0;
  std::vector<FlameNode> children;
  uint64_t samples = 0;  // number of samples that included this frame
};

struct FlameGraph {
  FlameNode root;
  uint64_t samples = 0;
  uint64_t total_weight = 0;
  uint64_t truncated_nodes = 0;
  bool truncated = false;
  TimeWindow window;
  bool derived_from_spans = false;
};

struct FlameOptions {
  size_t max_nodes = 50000;
  size_t max_depth = 128;
  uint64_t max_frame_length = 512;
};

/// Aggregates Sample records in [begin, end]. Frames are stored root-first.
/// When no samples exist, falls back to a span-duration flame graph so the UI
/// always has something meaningful to show (flagged in the result).
FlameGraph build_flame(const Trace& trace, TimeWindow window,
                       const FlameOptions& options = {});

/// Span-duration flame graph: nesting is reconstructed from span parents and
/// self time is duration minus children.
FlameGraph build_span_flame(const Trace& trace, TimeWindow window,
                            const FlameOptions& options = {});

// --- heap ownership graph and dominators -----------------------------------

struct HeapGraphNode {
  uint64_t ptr = 0;
  uint64_t size = 0;
  uint64_t retained = 0;
  uint64_t owner = 0;
  std::string site;
  std::string klass;
  int32_t idom = -1;  // index into nodes; -1 for roots
  std::vector<uint32_t> children;
};

struct HeapGraphOptions {
  size_t max_nodes = 20000;
  size_t max_edges = 100000;
};

struct HeapGraph {
  std::vector<HeapGraphNode> nodes;
  bool truncated = false;
  bool converged = true;
  size_t roots = 0;
  uint64_t total_bytes = 0;
};

/// Builds the ownership graph for live allocations at `at` and computes
/// retained sizes with an iterative dominator algorithm. Bounded by node and
/// edge limits; graphs that fail to converge are reported with converged=false
/// and retain the last safe result.
HeapGraph build_heap_graph(const Trace& trace, uint64_t at,
                           const HeapGraphOptions& options = {});

// --- diff ------------------------------------------------------------------

struct HeapSiteDelta {
  std::string site;
  std::string klass;
  uint64_t bytes_a = 0;
  uint64_t bytes_b = 0;
  uint64_t count_a = 0;
  uint64_t count_b = 0;
  int64_t bytes_delta = 0;
  int64_t count_delta = 0;
};

struct SpanNameDelta {
  std::string name;
  uint64_t count_a = 0;
  uint64_t count_b = 0;
  uint64_t total_ns_a = 0;
  uint64_t total_ns_b = 0;
  int64_t count_delta = 0;
  int64_t total_ns_delta = 0;
};

struct CounterDelta {
  std::string name;
  double before = 0;
  double after = 0;
  double delta = 0;
  bool present_before = false;
  bool present_after = false;
};

struct LogLevelDelta {
  LogLevel level = LogLevel::Info;
  uint64_t count_a = 0;
  uint64_t count_b = 0;
  int64_t delta = 0;
};

struct StateDelta {
  std::string key;
  std::string before;
  std::string after;
  bool changed = false;
};

struct DiffResult {
  uint64_t a = 0;
  uint64_t b = 0;
  uint64_t total_bytes_a = 0;
  uint64_t total_bytes_b = 0;
  int64_t total_bytes_delta = 0;
  uint64_t objects_a = 0;
  uint64_t objects_b = 0;
  int64_t objects_delta = 0;
  std::vector<HeapSiteDelta> heap;
  std::vector<SpanNameDelta> spans;
  std::vector<CounterDelta> counters;
  std::vector<LogLevelDelta> logs;
  std::vector<StateDelta> state;
};

/// Deterministic diff between two instants. `b` should be >= `a`; when it is
/// not, the values are still computed (callers can display a warning).
DiffResult diff(const Trace& trace, uint64_t a, uint64_t b, size_t limit = 200);

/// Renders a value for the UI/diagnostics.
std::string format_state_entry(const StateEntry& entry);

}  // namespace gbx
