// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "ui/serialize.hpp"

#include <string>

namespace gbx::ui {

void write_value(json::Writer& writer, const Trace& trace, const Value& value) {
  switch (value.type) {
    case ValueType::String:
      writer.value(trace.text(value.s));
      return;
    case ValueType::Int:
      writer.integer(value.i);
      return;
    case ValueType::Float:
      writer.number(value.f);
      return;
    case ValueType::Bool:
      writer.boolean(value.i != 0);
      return;
  }
  writer.null();
}

void write_record(json::Writer& writer, const Trace& trace,
                  const Record& record, uint32_t index) {
  writer.begin_object();
  writer.key_integer("id", static_cast<int64_t>(index));
  writer.key_integer("ts", static_cast<int64_t>(record.ts));
  writer.key_string("type", to_string(record.type));
  writer.key_integer("thread", static_cast<int64_t>(record.thread));
  const auto thread_name = trace.thread_name(record.thread);
  if (!thread_name.empty()) {
    writer.key_string("thread_name", thread_name);
  }
  writer.key_integer("span", static_cast<int64_t>(record.span));
  writer.key("fields");
  writer.begin_object();
  for (const auto& field : record.field_span(trace.fields)) {
    const auto key = trace.text(field.key);
    writer.key(key.empty() ? std::string_view("field") : key);
    write_value(writer, trace, field.value);
  }
  writer.end_object();
  writer.end_object();
}

std::string record_json(const Trace& trace, const Record& record,
                        uint32_t index) {
  json::Writer writer;
  write_record(writer, trace, record, index);
  return writer.take();
}

void write_live_record(json::Writer& writer, const LiveRecord& record) {
  writer.begin_object();
  writer.key_integer("ts", static_cast<int64_t>(record.ts));
  writer.key_string("type", to_string(record.type));
  writer.key_integer("thread", static_cast<int64_t>(record.thread));
  if (!record.thread_name.empty()) {
    writer.key_string("thread_name", record.thread_name);
  }
  writer.key("fields");
  writer.begin_object();
  for (const auto& field : record.fields) {
    writer.key(field.key);
    switch (field.value.type) {
      case ValueType::String:
        writer.value(field.text);
        break;
      case ValueType::Int:
        writer.integer(field.value.i);
        break;
      case ValueType::Float:
        writer.number(field.value.f);
        break;
      case ValueType::Bool:
        writer.boolean(field.value.i != 0);
        break;
    }
  }
  writer.end_object();
  writer.end_object();
}

std::string live_record_json(const LiveRecord& record) {
  json::Writer writer;
  write_live_record(writer, record);
  return writer.take();
}

}  // namespace gbx::ui
