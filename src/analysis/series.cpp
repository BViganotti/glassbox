// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "analysis/util.hpp"
#include "glassbox/analysis.hpp"

namespace gbx {
namespace {

void downsample(std::vector<SeriesPoint>& points, size_t max_points) {
  if (max_points < 2 || points.size() <= max_points) return;
  std::vector<SeriesPoint> sampled;
  sampled.reserve(max_points);
  const double step =
      static_cast<double>(points.size() - 1) / static_cast<double>(max_points - 1);
  for (size_t i = 0; i < max_points; ++i) {
    const size_t index = std::min<size_t>(
        static_cast<size_t>(static_cast<double>(i) * step + 0.5),
        points.size() - 1);
    sampled.push_back(points[index]);
  }
  points = std::move(sampled);
}

}  // namespace

std::vector<Series> counter_series(const Trace& trace,
                                   std::string_view name_filter,
                                   size_t max_series, size_t max_points) {
  std::map<std::string, Series> grouped;
  for (const auto& record : trace.records) {
    if (record.type != RecordType::Counter && record.type != RecordType::Gauge) {
      continue;
    }
    const auto name = trace.str_field(record, keys::kName);
    if (name.empty()) continue;
    if (!name_filter.empty() && !analysis::icontains(name, name_filter)) {
      continue;
    }
    auto& series = grouped[std::string(name)];
    series.name = std::string(name);
    series.kind = record.type;
    const auto unit = trace.str_field(record, keys::kUnit);
    if (!unit.empty()) series.unit = std::string(unit);
    series.points.push_back(
        {record.ts, trace.double_field(record, keys::kValue, 0.0)});
  }
  std::vector<Series> out;
  out.reserve(grouped.size());
  for (auto& [name, series] : grouped) {
    (void)name;
    if (series.points.empty()) continue;
    series.minimum = series.points.front().value;
    series.maximum = series.points.front().value;
    for (const auto& point : series.points) {
      series.minimum = std::min(series.minimum, point.value);
      series.maximum = std::max(series.maximum, point.value);
    }
    series.last = series.points.back().value;
    downsample(series.points, max_points);
    out.push_back(std::move(series));
  }
  if (out.size() > max_series) out.resize(max_series);
  return out;
}

bool series_value_at(const Trace& trace, std::string_view name, uint64_t at,
                     double& value) {
  bool found = false;
  for (const auto& record : trace.records) {
    if (record.ts > at) break;
    if (record.type != RecordType::Counter &&
        record.type != RecordType::Gauge) {
      continue;
    }
    if (trace.str_field(record, keys::kName) != name) continue;
    value = trace.double_field(record, keys::kValue, 0.0);
    found = true;
  }
  return found;
}

std::vector<StateEntry> state_at(const Trace& trace, uint64_t at) {
  std::map<std::string, StateEntry> state;
  const auto [first, last] = trace.record_range(0, at);
  for (size_t i = first; i < last; ++i) {
    const auto& record = trace.records[i];
    if (record.type != RecordType::Snapshot) continue;
    const auto fields = record.field_span(trace.fields);
    for (const auto& field : fields) {
      const auto key = trace.text(field.key);
      if (key.empty() || key.size() > 512) continue;
      StateEntry entry;
      entry.key = std::string(key);
      entry.ts = record.ts;
      switch (field.value.type) {
        case ValueType::String:
          entry.type = ValueType::String;
          entry.text = std::string(trace.text(field.value.s));
          break;
        case ValueType::Int:
          entry.type = ValueType::Float;
          entry.number = static_cast<double>(field.value.i);
          break;
        case ValueType::Float:
          entry.type = ValueType::Float;
          entry.number = field.value.f;
          break;
        case ValueType::Bool:
          entry.type = ValueType::Float;
          entry.number = field.value.i != 0 ? 1.0 : 0.0;
          break;
      }
      state[entry.key] = std::move(entry);
    }
  }
  std::vector<StateEntry> out;
  out.reserve(state.size());
  for (auto& [key, entry] : state) {
    (void)key;
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<StatePoint> state_history(const Trace& trace, std::string_view key,
                                      size_t limit, uint64_t from,
                                      uint64_t to) {
  std::vector<StatePoint> points;
  if (key.empty() || limit == 0) return points;
  limit = std::min<size_t>(limit, 10000);
  const auto [first, last] = trace.record_range(from, to);
  for (size_t i = first; i < last; ++i) {
    const auto& record = trace.records[i];
    if (record.type != RecordType::Snapshot) continue;
    for (const auto& field : record.field_span(trace.fields)) {
      if (trace.text(field.key) != key) continue;
      StatePoint point;
      point.ts = record.ts;
      switch (field.value.type) {
        case ValueType::String:
          point.type = ValueType::String;
          point.text = std::string(trace.text(field.value.s));
          break;
        case ValueType::Int:
          point.type = ValueType::Float;
          point.number = static_cast<double>(field.value.i);
          break;
        case ValueType::Float:
          point.type = ValueType::Float;
          point.number = field.value.f;
          break;
        case ValueType::Bool:
          point.type = ValueType::Float;
          point.number = field.value.i != 0 ? 1.0 : 0.0;
          break;
      }
      points.push_back(std::move(point));
    }
  }
  if (points.size() > limit) {
    std::vector<StatePoint> sampled;
    sampled.reserve(limit);
    const double step =
        static_cast<double>(points.size() - 1) / static_cast<double>(limit - 1);
    for (size_t i = 0; i < limit; ++i) {
      const size_t index = std::min<size_t>(
          static_cast<size_t>(static_cast<double>(i) * step + 0.5),
          points.size() - 1);
      sampled.push_back(points[index]);
    }
    points = std::move(sampled);
  }
  return points;
}

std::string format_state_entry(const StateEntry& entry) {
  if (entry.type == ValueType::String) return entry.text;
  return analysis::format_double(entry.number);
}

}  // namespace gbx
