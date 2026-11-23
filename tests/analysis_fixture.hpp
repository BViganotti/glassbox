// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "glassbox/trace.hpp"

namespace gbx::test {

/// Small builder for hand-crafted traces used across analysis tests.
struct AnalysisFixture {
  gbx::Trace trace;
  std::vector<gbx::Field> scratch;
  uint32_t next_span = 1;

  uint32_t str(std::string_view text) {
    auto id = trace.strings.try_intern(text);
    return id.value_or(0);
  }

  void emit(uint64_t ts, gbx::RecordType type, uint32_t thread = 1,
            uint32_t span = 0) {
    gbx::Record record;
    record.ts = ts;
    record.type = type;
    record.thread = thread;
    record.span = span;
    trace.add_record(record, scratch);
    scratch.clear();
  }

  uint32_t begin_span(std::string_view name, uint64_t ts,
                      uint32_t parent = 0, uint32_t thread = 1) {
    const uint32_t id = next_span++;
    scratch.push_back({str("id"), gbx::Value::integer(id)});
    scratch.push_back({str("parent"), gbx::Value::integer(parent)});
    scratch.push_back({str("name"), gbx::Value::str(str(name))});
    scratch.push_back({str("kind"), gbx::Value::str(str("cpu"))});
    emit(ts, gbx::RecordType::SpanBegin, thread, id);
    return id;
  }

  void end_span(uint32_t id, uint64_t ts, uint32_t thread = 1,
                uint32_t parent = 0) {
    scratch.push_back({str("id"), gbx::Value::integer(id)});
    emit(ts, gbx::RecordType::SpanEnd, thread, parent);
  }

  void alloc(uint64_t ts, uint64_t ptr, uint64_t size, std::string_view site,
             std::string_view klass, uint64_t owner = 0) {
    scratch.push_back({str("ptr"), gbx::Value::integer(static_cast<int64_t>(ptr))});
    scratch.push_back({str("size"), gbx::Value::integer(static_cast<int64_t>(size))});
    scratch.push_back({str("site"), gbx::Value::str(str(site))});
    scratch.push_back({str("class"), gbx::Value::str(str(klass))});
    if (owner != 0) {
      scratch.push_back({str("owner"),
                         gbx::Value::integer(static_cast<int64_t>(owner))});
    }
    emit(ts, gbx::RecordType::Alloc);
  }

  void free(uint64_t ts, uint64_t ptr) {
    scratch.push_back({str("ptr"), gbx::Value::integer(static_cast<int64_t>(ptr))});
    emit(ts, gbx::RecordType::Free);
  }

  void log(uint64_t ts, gbx::LogLevel level, std::string_view message,
           std::string_view category = {}) {
    scratch.push_back({str("level"),
                       gbx::Value::integer(static_cast<int64_t>(level))});
    scratch.push_back({str("msg"), gbx::Value::str(str(message))});
    if (!category.empty()) {
      scratch.push_back({str("category"), gbx::Value::str(str(category))});
    }
    emit(ts, gbx::RecordType::Log);
  }

  void sample(uint64_t ts, std::initializer_list<std::string_view> frames,
              double weight = 1.0) {
    std::string joined;
    for (std::string_view frame : frames) {
      if (!joined.empty()) joined.push_back(gbx::kFrameSeparator);
      joined.append(frame);
    }
    scratch.push_back({str("frames"), gbx::Value::str(str(joined))});
    scratch.push_back({str("weight"), gbx::Value::real(weight)});
    emit(ts, gbx::RecordType::Sample);
  }

  void counter(uint64_t ts, std::string_view name, double value,
               std::string_view unit = {}) {
    scratch.push_back({str("name"), gbx::Value::str(str(name))});
    scratch.push_back({str("value"), gbx::Value::real(value)});
    if (!unit.empty()) {
      scratch.push_back({str("unit"), gbx::Value::str(str(unit))});
    }
    emit(ts, gbx::RecordType::Counter);
  }

  void gauge(uint64_t ts, std::string_view name, double value) {
    scratch.push_back({str("name"), gbx::Value::str(str(name))});
    scratch.push_back({str("value"), gbx::Value::real(value)});
    emit(ts, gbx::RecordType::Gauge);
  }

  void snapshot_number(uint64_t ts, std::string_view key, double value) {
    scratch.push_back({str(key), gbx::Value::real(value)});
    emit(ts, gbx::RecordType::Snapshot);
  }

  void snapshot_text(uint64_t ts, std::string_view key, std::string_view value) {
    scratch.push_back({str(key), gbx::Value::str(str(value))});
    emit(ts, gbx::RecordType::Snapshot);
  }

  void marker(uint64_t ts, std::string_view label) {
    scratch.push_back({str("label"), gbx::Value::str(str(label))});
    emit(ts, gbx::RecordType::Marker);
  }

  void finalize() { trace.finalize(); }
};

}  // namespace gbx::test
