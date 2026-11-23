// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "glassbox/analysis.hpp"

namespace gbx {
namespace {

std::string key_of(std::string_view site, std::string_view klass) {
  std::string key(site);
  key.push_back('\x1f');
  key.append(klass);
  return key;
}

int64_t abs_i64(int64_t value) { return value < 0 ? -value : value; }

}  // namespace

DiffResult diff(const Trace& trace, uint64_t a, uint64_t b, size_t limit) {
  limit = std::min<size_t>(limit == 0 ? 1 : limit, 2000);
  DiffResult result;
  result.a = a;
  result.b = b;

  HeapOptions heap_options;
  heap_options.max_sites = 4096;
  heap_options.max_objects = 1;
  const HeapAt heap_a = heap_at(trace, a, heap_options);
  const HeapAt heap_b = heap_at(trace, b, heap_options);
  result.total_bytes_a = heap_a.total_bytes;
  result.total_bytes_b = heap_b.total_bytes;
  result.total_bytes_delta = static_cast<int64_t>(heap_b.total_bytes) -
                             static_cast<int64_t>(heap_a.total_bytes);
  result.objects_a = heap_a.object_count;
  result.objects_b = heap_b.object_count;
  result.objects_delta =
      static_cast<int64_t>(heap_b.object_count) -
      static_cast<int64_t>(heap_a.object_count);

  struct SitePair {
    HeapSiteStat a;
    HeapSiteStat b;
    bool has_a = false;
    bool has_b = false;
  };
  std::unordered_map<std::string, SitePair> site_map;
  for (const auto& site : heap_a.sites) {
    auto& pair = site_map[key_of(site.site, site.klass)];
    pair.a = site;
    pair.has_a = true;
  }
  for (const auto& site : heap_b.sites) {
    auto& pair = site_map[key_of(site.site, site.klass)];
    pair.b = site;
    pair.has_b = true;
  }
  for (auto& [key, pair] : site_map) {
    (void)key;
    if (!pair.has_a && !pair.has_b) continue;
    if (pair.has_a && pair.has_b && pair.a.live_bytes == pair.b.live_bytes &&
        pair.a.live_count == pair.b.live_count) {
      continue;
    }
    HeapSiteDelta delta;
    delta.site = pair.has_b ? pair.b.site : pair.a.site;
    delta.klass = pair.has_b ? pair.b.klass : pair.a.klass;
    delta.bytes_a = pair.a.live_bytes;
    delta.bytes_b = pair.b.live_bytes;
    delta.count_a = pair.a.live_count;
    delta.count_b = pair.b.live_count;
    delta.bytes_delta = static_cast<int64_t>(delta.bytes_b) -
                        static_cast<int64_t>(delta.bytes_a);
    delta.count_delta = static_cast<int64_t>(delta.count_b) -
                        static_cast<int64_t>(delta.count_a);
    result.heap.push_back(std::move(delta));
  }
  std::sort(result.heap.begin(), result.heap.end(),
            [](const HeapSiteDelta& x, const HeapSiteDelta& y) {
              const int64_t ax = abs_i64(x.bytes_delta);
              const int64_t ay = abs_i64(y.bytes_delta);
              if (ax != ay) return ax > ay;
              if (x.site != y.site) return x.site < y.site;
              return x.klass < y.klass;
            });
  if (result.heap.size() > limit) result.heap.resize(limit);

  const SpanAnalysis spans = analyze_spans(trace);
  struct SpanPair {
    uint64_t count_a = 0;
    uint64_t count_b = 0;
    uint64_t ns_a = 0;
    uint64_t ns_b = 0;
  };
  std::map<std::string, SpanPair> span_map;
  for (const auto& span : spans.spans) {
    auto& pair = span_map[span.name];
    const uint64_t duration = span.duration(trace.end_ts());
    if (span.begin_ts <= a) {
      ++pair.count_a;
      pair.ns_a += duration;
    }
    if (span.begin_ts <= b) {
      ++pair.count_b;
      pair.ns_b += duration;
    }
  }
  for (auto& [name, pair] : span_map) {
    if (pair.count_a == pair.count_b && pair.ns_a == pair.ns_b) continue;
    SpanNameDelta delta;
    delta.name = name;
    delta.count_a = pair.count_a;
    delta.count_b = pair.count_b;
    delta.total_ns_a = pair.ns_a;
    delta.total_ns_b = pair.ns_b;
    delta.count_delta = static_cast<int64_t>(pair.count_b) -
                        static_cast<int64_t>(pair.count_a);
    delta.total_ns_delta = static_cast<int64_t>(pair.ns_b) -
                           static_cast<int64_t>(pair.ns_a);
    result.spans.push_back(std::move(delta));
  }
  std::sort(result.spans.begin(), result.spans.end(),
            [](const SpanNameDelta& x, const SpanNameDelta& y) {
              const int64_t ax = abs_i64(x.total_ns_delta);
              const int64_t ay = abs_i64(y.total_ns_delta);
              if (ax != ay) return ax > ay;
              return x.name < y.name;
            });
  if (result.spans.size() > limit) result.spans.resize(limit);

  // Cumulative log counts let the UI show what appeared between the moments.
  std::map<LogLevel, std::pair<uint64_t, uint64_t>> log_counts;
  for (const auto& record : trace.records) {
    if (record.type != RecordType::Log) continue;
    const int64_t raw = trace.int_field(record, keys::kLevel, 0);
    const auto level = static_cast<LogLevel>(
        std::clamp<int64_t>(raw, 0, static_cast<int64_t>(LogLevel::Fatal)));
    auto& counts = log_counts[level];
    if (record.ts <= a) ++counts.first;
    if (record.ts <= b) ++counts.second;
  }
  for (auto& [level, counts] : log_counts) {
    if (counts.first == counts.second) continue;
    LogLevelDelta delta;
    delta.level = level;
    delta.count_a = counts.first;
    delta.count_b = counts.second;
    delta.delta = static_cast<int64_t>(counts.second) -
                  static_cast<int64_t>(counts.first);
    result.logs.push_back(delta);
  }
  std::sort(result.logs.begin(), result.logs.end(),
            [](const LogLevelDelta& x, const LogLevelDelta& y) {
              return static_cast<uint8_t>(x.level) <
                     static_cast<uint8_t>(y.level);
            });

  std::vector<Series> series = counter_series(trace, {}, 256, 4);
  for (const auto& s : series) {
    double before = 0;
    double after = 0;
    const bool has_before = series_value_at(trace, s.name, a, before);
    const bool has_after = series_value_at(trace, s.name, b, after);
    if (!has_before && !has_after) continue;
    const double delta = after - before;
    if (has_before == has_after && delta == 0.0) continue;
    CounterDelta item;
    item.name = s.name;
    item.before = before;
    item.after = after;
    item.delta = delta;
    item.present_before = has_before;
    item.present_after = has_after;
    result.counters.push_back(std::move(item));
  }
  std::sort(result.counters.begin(), result.counters.end(),
            [](const CounterDelta& x, const CounterDelta& y) {
              return x.name < y.name;
            });
  if (result.counters.size() > limit) result.counters.resize(limit);

  const auto state_a = state_at(trace, a);
  const auto state_b = state_at(trace, b);
  std::map<std::string, std::pair<const StateEntry*, const StateEntry*>> state_map;
  for (const auto& entry : state_a) {
    state_map[entry.key].first = &entry;
  }
  for (const auto& entry : state_b) {
    state_map[entry.key].second = &entry;
  }
  for (auto& [key, pair] : state_map) {
    (void)key;
    if (pair.first == nullptr && pair.second == nullptr) continue;
    const std::string before =
        pair.first ? format_state_entry(*pair.first) : std::string();
    const std::string after =
        pair.second ? format_state_entry(*pair.second) : std::string();
    if (before == after && pair.first && pair.second) continue;
    StateDelta delta;
    delta.key = key;
    delta.before = before;
    delta.after = after;
    delta.changed = before != after;
    result.state.push_back(std::move(delta));
  }
  std::sort(result.state.begin(), result.state.end(),
            [](const StateDelta& x, const StateDelta& y) {
              return x.key < y.key;
            });
  if (result.state.size() > limit) result.state.resize(limit);

  return result;
}

}  // namespace gbx
