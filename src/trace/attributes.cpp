// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/attributes.hpp"

#include <string>

namespace gbx {

std::string_view to_string(RecordType type) {
  switch (type) {
    case RecordType::Meta: return "meta";
    case RecordType::ThreadName: return "thread";
    case RecordType::SpanBegin: return "span_begin";
    case RecordType::SpanEnd: return "span_end";
    case RecordType::Log: return "log";
    case RecordType::Counter: return "counter";
    case RecordType::Gauge: return "gauge";
    case RecordType::Alloc: return "alloc";
    case RecordType::Free: return "free";
    case RecordType::Snapshot: return "snapshot";
    case RecordType::Sample: return "sample";
    case RecordType::Marker: return "marker";
    case RecordType::Custom: return "custom";
  }
  return "unknown";
}

std::string_view to_string(LogLevel level) {
  switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Fatal: return "fatal";
  }
  return "unknown";
}

bool parse_record_type(std::string_view text, RecordType& out) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(RecordType::Custom); ++i) {
    const auto type = static_cast<RecordType>(i);
    if (to_string(type) == text) {
      out = type;
      return true;
    }
  }
  return false;
}

bool parse_log_level(std::string_view text, LogLevel& out) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(LogLevel::Fatal); ++i) {
    const auto level = static_cast<LogLevel>(i);
    if (to_string(level) == text) {
      out = level;
      return true;
    }
  }
  return false;
}

std::string format_value(const Value& value) {
  switch (value.type) {
    case ValueType::Int: return std::to_string(value.i);
    case ValueType::Float: return std::to_string(value.f);
    case ValueType::String: return "#" + std::to_string(value.s);
    case ValueType::Bool: return value.i != 0 ? "true" : "false";
  }
  return "?";
}

}  // namespace gbx
