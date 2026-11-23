// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "ui/routes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "glassbox/analysis.hpp"
#include "glassbox/json.hpp"
#include "glassbox/query.hpp"
#include "ui/serialize.hpp"

namespace gbx::ui {
namespace {

constexpr uint32_t kMaxEvents = 5000;
constexpr uint32_t kMaxFlameNodes = 100000;

uint64_t parse_u64(std::string_view text, uint64_t fallback, uint64_t limit) {
  if (text.empty()) return fallback;
  uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return fallback;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (limit - digit) / 10) return limit;
    value = value * 10 + digit;
  }
  return std::min(value, limit);
}

bool flag(const HttpRequest& request, std::string_view name) {
  const auto value = request.query_value(name);
  return value == "1" || value == "true" || value == "yes";
}

uint64_t param_at(const HttpRequest& request, const Trace& trace) {
  constexpr uint64_t kMax = 1ull << 62;
  const uint64_t at = parse_u64(request.query_value("at"), trace.end_ts(), kMax);
  return std::min(at, trace.end_ts());
}

TimeWindow window_of(const HttpRequest& request, const Trace& trace) {
  const uint64_t end_limit = trace.end_ts();
  TimeWindow window;
  window.begin = std::min(
      parse_u64(request.query_value("from"), 0, end_limit), end_limit);
  window.end = std::min(
      parse_u64(request.query_value("to"), end_limit, end_limit), end_limit);
  if (window.end < window.begin) window.end = window.begin;
  return window;
}

std::string hex_pointer(uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

std::string format_ns(uint64_t ns) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.3fms",
                static_cast<double>(ns) / 1e6);
  return buffer;
}

void write_span(json::Writer& writer, const SpanInfo& span) {
  writer.begin_object();
  writer.key_integer("id", static_cast<int64_t>(span.id));
  writer.key_integer("parent", static_cast<int64_t>(span.parent));
  writer.key_integer("thread", static_cast<int64_t>(span.thread));
  writer.key_integer("depth", static_cast<int64_t>(span.depth));
  writer.key_integer("begin_ts", static_cast<int64_t>(span.begin_ts));
  writer.key_integer("end_ts", static_cast<int64_t>(span.closed ? span.end_ts
                                                               : span.begin_ts));
  writer.key_boolean("closed", span.closed);
  writer.key_string("name", span.name);
  writer.key_string("kind", span.kind);
  writer.end_object();
}

void write_flame_node(json::Writer& writer, const FlameNode& node,
                      size_t& remaining, bool& truncated) {
  --remaining;
  writer.begin_object();
  writer.key_string("name", node.name);
  writer.key_integer("self", static_cast<int64_t>(node.self));
  writer.key_integer("total", static_cast<int64_t>(node.total));
  writer.key_integer("samples", static_cast<int64_t>(node.samples));
  writer.key("children");
  writer.begin_array();
  for (const auto& child : node.children) {
    if (remaining == 0) {
      truncated = true;
      break;
    }
    write_flame_node(writer, child, remaining, truncated);
  }
  writer.end_array();
  writer.end_object();
}

std::vector<uint32_t> select_records(const Trace& trace, uint64_t begin,
                                     uint64_t end, uint32_t limit, bool newest,
                                     const Query* query, uint16_t type_mask,
                                     int thread, int min_level,
                                     bool& truncated) {
  std::vector<uint32_t> out;
  truncated = false;
  const auto [first, last] = trace.record_range(begin, end);
  if (first >= last) return out;
  auto matches = [&](const Record& record) {
    if (type_mask != 0 &&
        (type_mask & (1u << static_cast<uint32_t>(record.type))) == 0) {
      return false;
    }
    if (thread >= 0 && static_cast<int>(record.thread) != thread) return false;
    if (min_level >= 0) {
      if (record.type != RecordType::Log) return false;
      if (trace.int_field(record, keys::kLevel, 0) < min_level) return false;
    }
    if (query != nullptr && !query->matches(trace, record)) return false;
    return true;
  };
  if (newest) {
    for (size_t i = last; i > first; --i) {
      if (!matches(trace.records[i - 1])) continue;
      out.push_back(static_cast<uint32_t>(i - 1));
      if (out.size() >= limit) {
        // Check whether there is at least one more match for the flag.
        for (size_t j = i - 1; j > first; --j) {
          if (matches(trace.records[j - 1])) {
            truncated = true;
            break;
          }
        }
        break;
      }
    }
  } else {
    for (size_t i = first; i < last; ++i) {
      if (!matches(trace.records[i])) continue;
      out.push_back(static_cast<uint32_t>(i));
      if (out.size() >= limit) {
        for (size_t j = i + 1; j < last; ++j) {
          if (matches(trace.records[j])) {
            truncated = true;
            break;
          }
        }
        break;
      }
    }
  }
  return out;
}

bool parse_type_mask(std::string_view text, uint16_t& mask,
                     std::string& error) {
  mask = 0;
  size_t pos = 0;
  while (pos <= text.size() && !text.empty()) {
    const size_t comma = text.find(',', pos);
    const size_t end = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view item = text.substr(pos, end - pos);
    if (!item.empty()) {
      RecordType type;
      if (!parse_record_type(item, type)) {
        error = "unknown record type: " + std::string(item);
        return false;
      }
      mask |= static_cast<uint16_t>(1u << static_cast<uint32_t>(type));
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return true;
}

struct ParsedQuery {
  Query query;
  bool provided = false;
  bool valid = true;
  QueryError error;
};

ParsedQuery parse_query_param(const HttpRequest& request) {
  ParsedQuery parsed;
  const auto text = request.query_value("q");
  if (text.empty()) return parsed;
  parsed.provided = true;
  parsed.query = Query::parse(text, &parsed.error);
  parsed.valid = parsed.query.valid();
  return parsed;
}

void send_query_error(HttpResponse& response, const QueryError& error) {
  response.status = 400;
  response.content_type = "application/json; charset=utf-8";
  json::Writer writer;
  writer.begin_object();
  writer.key_string("error", error.message);
  writer.key_integer("offset", static_cast<int64_t>(error.offset));
  writer.end_object();
  response.body = writer.str();
}

void write_trace_meta(json::Writer& writer, const TraceSnapshot& snapshot) {
  const Trace& trace = *snapshot.trace;
  writer.begin_object();
  writer.key_string("glassbox_version", GBX_VERSION);
  writer.key_string("name", snapshot.name);
  writer.key_string("path", snapshot.path);
  writer.key_integer("bytes", static_cast<int64_t>(snapshot.bytes));
  writer.key_integer("mtime", snapshot.mtime);
  writer.key_string("app", trace.meta.app_name);
  writer.key_string("version", trace.meta.app_version);
  writer.key_string("exe", trace.meta.exe_path);
  writer.key_string("arch", trace.meta.arch);
  writer.key_string("os", trace.meta.os);
  writer.key_integer("pid", static_cast<int64_t>(trace.meta.pid));
  writer.key_integer("wall_start_ns",
                     static_cast<int64_t>(trace.meta.wall_start_ns));
  writer.key_integer("duration_ns", static_cast<int64_t>(trace.end_ts()));
  writer.key_integer("records", static_cast<int64_t>(trace.records.size()));
  writer.key_integer("fields", static_cast<int64_t>(trace.fields.size()));
  writer.key_integer("strings", static_cast<int64_t>(trace.strings.size()));
  writer.key_integer("chunks", static_cast<int64_t>(trace.chunks.size()));
  writer.key_boolean("truncated", trace.diagnostics.truncated);
  writer.key_string("warning", trace.diagnostics.warning);
  writer.key_integer("bytes_read",
                     static_cast<int64_t>(trace.diagnostics.bytes_read));
  writer.key_integer("memory_bytes",
                     static_cast<int64_t>(trace.memory_usage()));
  writer.key("threads");
  writer.begin_array();
  for (uint32_t i = 0; i < trace.thread_count(); ++i) {
    const auto name = trace.thread_name(i);
    if (name.empty()) continue;
    writer.begin_object();
    writer.key_integer("id", static_cast<int64_t>(i));
    writer.key_string("name", name);
    writer.end_object();
  }
  writer.end_array();
  writer.key("types");
  writer.begin_object();
  for (uint8_t i = 0; i <= static_cast<uint8_t>(RecordType::Custom); ++i) {
    const auto type = static_cast<RecordType>(i);
    size_t count = 0;
    for (const auto& record : trace.records) {
      if (record.type == type) ++count;
    }
    writer.key_integer(to_string(type), static_cast<int64_t>(count));
  }
  writer.end_object();
  writer.key("markers");
  writer.begin_array();
  size_t markers = 0;
  for (size_t i = 0; i < trace.records.size() && markers < 200; ++i) {
    const auto& record = trace.records[i];
    if (record.type != RecordType::Marker) continue;
    const auto label = trace.str_field(record, keys::kLabel);
    if (label.empty()) continue;
    writer.begin_object();
    writer.key_integer("ts", static_cast<int64_t>(record.ts));
    writer.key_string("label", label);
    writer.end_object();
    ++markers;
  }
  writer.end_array();
  writer.key("counters");
  writer.begin_array();
  for (const auto& series : counter_series(trace, {}, 64, 2)) {
    writer.begin_object();
    writer.key_string("name", series.name);
    writer.key_string("unit", series.unit);
    writer.key_string("kind", to_string(series.kind));
    writer.key_number("min", series.minimum);
    writer.key_number("max", series.maximum);
    writer.key_number("last", series.last);
    writer.end_object();
  }
  writer.end_array();
  writer.key("state_keys");
  writer.begin_array();
  for (const auto& entry : state_at(trace, trace.end_ts())) {
    if (writer.str().size() > 256 * 1024) break;
    writer.value(entry.key);
  }
  writer.end_array();
  writer.key("leak_count");
  writer.integer(static_cast<int64_t>(leak_candidates(trace, 1000).size()));
  writer.end_object();
}

}  // namespace

void register_trace_routes(HttpServer& server, TraceGetter getter,
                           Guard guard) {
  auto get = [guard,
              getter](const HttpRequest& request,
                      HttpResponse& response) -> std::shared_ptr<const Trace> {
    if (!guard(request, response)) return nullptr;
    TraceSnapshot snapshot = getter();
    if (!snapshot.trace) {
      response.set_error(404, "no trace is open");
      return nullptr;
    }
    return snapshot.trace;
  };

  auto add = [&server, &guard](std::string_view path, HttpServer::Handler handler) {
    server.route("GET", path,
                 [guard, handler = std::move(handler)](
                     const HttpRequest& request, HttpResponse& response) {
                   if (!guard(request, response)) return;
                   handler(request, response);
                 });
  };

  add("/api/meta", [get, getter](const HttpRequest& request,
                                 HttpResponse& response) {
    if (!get(request, response)) return;
    const TraceSnapshot snapshot = getter();
    json::Writer writer;
    write_trace_meta(writer, snapshot);
    response.set_body(writer.str());
  });

  add("/api/timeline", [get](const HttpRequest& request,
                            HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint32_t buckets = static_cast<uint32_t>(
        parse_u64(request.query_value("buckets"), 160, 512));
    const TimeWindow window = window_of(request, *trace);
    const Timeline timeline = build_timeline(*trace, buckets, window);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("begin", static_cast<int64_t>(timeline.begin_ts));
    writer.key_integer("end", static_cast<int64_t>(timeline.end_ts));
    writer.key_integer("bucket_width",
                       static_cast<int64_t>(timeline.bucket_width));
    writer.key_number("max_span_ns", timeline.max_span_ns);
    writer.key_integer("max_alloc_bytes",
                       static_cast<int64_t>(timeline.max_alloc_bytes));
    writer.key_integer("total_alloc_bytes",
                       static_cast<int64_t>(timeline.total_alloc_bytes));
    writer.key_integer("total_logs", static_cast<int64_t>(timeline.total_logs));
    writer.key_integer("total_errors",
                       static_cast<int64_t>(timeline.total_errors));
    writer.key_integer("total_samples",
                       static_cast<int64_t>(timeline.total_samples));
    writer.key("buckets");
    writer.begin_array();
    for (const auto& bucket : timeline.buckets) {
      writer.begin_object();
      writer.key_integer("begin", static_cast<int64_t>(bucket.begin_ts));
      writer.key_integer("end", static_cast<int64_t>(bucket.end_ts));
      writer.key_number("span_ns", bucket.span_ns);
      writer.key_integer("alloc_bytes",
                         static_cast<int64_t>(bucket.alloc_bytes));
      writer.key_integer("alloc_count",
                         static_cast<int64_t>(bucket.alloc_count));
      writer.key_integer("free_count", static_cast<int64_t>(bucket.free_count));
      writer.key_integer("log_count", static_cast<int64_t>(bucket.log_count));
      writer.key_integer("error_count",
                         static_cast<int64_t>(bucket.error_count));
      writer.key_integer("sample_count",
                         static_cast<int64_t>(bucket.sample_count));
      writer.key_integer("span_starts",
                         static_cast<int64_t>(bucket.span_starts));
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/events", [get](const HttpRequest& request,
                           HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const TimeWindow window = window_of(request, *trace);
    const uint32_t limit = static_cast<uint32_t>(
        parse_u64(request.query_value("limit"), 200, kMaxEvents));
    const bool newest = flag(request, "newest");
    const auto thread_text = request.query_value("thread");
    int thread = -1;
    if (!thread_text.empty()) {
      thread = static_cast<int>(parse_u64(thread_text, 0, 1u << 20));
    }
    uint16_t mask = 0;
    std::string type_error;
    const auto types = request.query_value("types");
    if (!types.empty() && !parse_type_mask(types, mask, type_error)) {
      response.set_error(400, type_error);
      return;
    }
    const ParsedQuery parsed = parse_query_param(request);
    if (!parsed.valid) {
      send_query_error(response, parsed.error);
      return;
    }
    bool truncated = false;
    const auto ids = select_records(
        *trace, window.begin, window.end, limit, newest,
        parsed.provided ? &parsed.query : nullptr, mask, thread, -1, truncated);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("from", static_cast<int64_t>(window.begin));
    writer.key_integer("to", static_cast<int64_t>(window.end));
    writer.key_integer("returned", static_cast<int64_t>(ids.size()));
    writer.key_boolean("truncated", truncated);
    writer.key("records");
    writer.begin_array();
    for (uint32_t id : ids) {
      write_record(writer, *trace, trace->records[id], id);
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/event", [get](const HttpRequest& request, HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint64_t id = parse_u64(request.query_value("id"),
                                  std::numeric_limits<uint64_t>::max(),
                                  std::numeric_limits<uint64_t>::max());
    if (id >= trace->records.size()) {
      response.set_error(404, "event not found");
      return;
    }
    json::Writer writer;
    write_record(writer, *trace, trace->records[static_cast<size_t>(id)],
                 static_cast<uint32_t>(id));
    response.set_body(writer.str());
  });

  add("/api/logs", [get](const HttpRequest& request, HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const TimeWindow window = window_of(request, *trace);
    const uint32_t limit = static_cast<uint32_t>(
        parse_u64(request.query_value("limit"), 300, kMaxEvents));
    const bool newest = flag(request, "newest");
    const int min_level = static_cast<int>(
        parse_u64(request.query_value("level"), 0, 5));
    const ParsedQuery parsed = parse_query_param(request);
    if (!parsed.valid) {
      send_query_error(response, parsed.error);
      return;
    }
    bool truncated = false;
    const auto ids = select_records(
        *trace, window.begin, window.end, limit, newest,
        parsed.provided ? &parsed.query : nullptr,
        static_cast<uint16_t>(1u << static_cast<uint32_t>(RecordType::Log)),
        -1, min_level, truncated);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("from", static_cast<int64_t>(window.begin));
    writer.key_integer("to", static_cast<int64_t>(window.end));
    writer.key_integer("returned", static_cast<int64_t>(ids.size()));
    writer.key_boolean("truncated", truncated);
    writer.key("records");
    writer.begin_array();
    for (uint32_t id : ids) {
      write_record(writer, *trace, trace->records[id], id);
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/query", [get](const HttpRequest& request,
                          HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const auto text = request.query_value("q");
    QueryError error;
    auto query = Query::parse(text, &error);
    if (!query.valid()) {
      send_query_error(response, error);
      return;
    }
    const TimeWindow window = window_of(request, *trace);
    const uint32_t limit = static_cast<uint32_t>(
        parse_u64(request.query_value("limit"), 300, kMaxEvents));
    const bool newest = flag(request, "newest");
    bool truncated = false;
    const auto ids = select_records(*trace, window.begin, window.end, limit,
                                    newest, &query, 0, -1, -1, truncated);
    json::Writer writer;
    writer.begin_object();
    writer.key_string("query", query.source());
    writer.key_string("describe", query.describe());
    writer.key_integer("from", static_cast<int64_t>(window.begin));
    writer.key_integer("to", static_cast<int64_t>(window.end));
    writer.key_integer("returned", static_cast<int64_t>(ids.size()));
    writer.key_boolean("truncated", truncated);
    writer.key("records");
    writer.begin_array();
    for (uint32_t id : ids) {
      write_record(writer, *trace, trace->records[id], id);
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/spans", [get](const HttpRequest& request,
                          HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    if (!request.query_value("at").empty()) {
      const uint64_t at = param_at(request, *trace);
      const auto active = active_spans(*trace, at);
      json::Writer writer;
      writer.begin_object();
      writer.key_integer("at", static_cast<int64_t>(at));
      writer.key_integer("count", static_cast<int64_t>(active.size()));
      writer.key("spans");
      writer.begin_array();
      for (const auto& span : active) {
        writer.begin_object();
        writer.key_integer("id", static_cast<int64_t>(span.id));
        writer.key_integer("parent", static_cast<int64_t>(span.parent));
        writer.key_integer("thread", static_cast<int64_t>(span.thread));
        writer.key_integer("depth", static_cast<int64_t>(span.depth));
        writer.key_integer("begin_ts", static_cast<int64_t>(span.begin_ts));
        writer.key_string("name", span.name);
        writer.key_string("kind", span.kind);
        writer.end_object();
      }
      writer.end_array();
      writer.end_object();
      response.set_body(writer.str());
      return;
    }
    const TimeWindow window = window_of(request, *trace);
    const size_t limit = static_cast<size_t>(
        parse_u64(request.query_value("limit"), 2000, 20000));
    const SpanAnalysis analysis = analyze_spans(*trace);
    std::vector<const SpanInfo*> selected;
    selected.reserve(std::min(analysis.spans.size(), limit));
    for (const auto& span : analysis.spans) {
      if (span.begin_ts < window.begin || span.begin_ts > window.end) continue;
      if (selected.size() >= limit) break;
      selected.push_back(&span);
    }
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("from", static_cast<int64_t>(window.begin));
    writer.key_integer("to", static_cast<int64_t>(window.end));
    writer.key_integer("total", static_cast<int64_t>(analysis.spans.size()));
    writer.key_integer("returned", static_cast<int64_t>(selected.size()));
    writer.key_integer("unmatched_ends",
                       static_cast<int64_t>(analysis.unmatched_ends));
    writer.key_integer("unclosed", static_cast<int64_t>(analysis.unclosed));
    writer.key("spans");
    writer.begin_array();
    for (const SpanInfo* span : selected) write_span(writer, *span);
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/span", [get](const HttpRequest& request, HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint64_t id = parse_u64(request.query_value("id"), 0, 0xffffffffu);
    const SpanAnalysis analysis = analyze_spans(*trace);
    auto it = analysis.by_id.find(static_cast<uint32_t>(id));
    if (it == analysis.by_id.end()) {
      response.set_error(404, "span not found");
      return;
    }
    const SpanInfo& span = analysis.spans[it->second];
    json::Writer writer;
    writer.begin_object();
    write_span(writer, span);
    writer.key_integer("duration_ns",
                       static_cast<int64_t>(span.duration(trace->end_ts())));
    writer.key("children");
    writer.begin_array();
    for (uint32_t child : analysis.children[it->second]) {
      writer.integer(static_cast<int64_t>(analysis.spans[child].id));
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/heap", [get](const HttpRequest& request,
                         HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint64_t at = param_at(request, *trace);
    HeapOptions options;
    options.max_objects = static_cast<size_t>(
        parse_u64(request.query_value("limit"), 500, 20000));
    options.max_sites = 256;
    const HeapAt heap = heap_at(*trace, at, options);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("at", static_cast<int64_t>(heap.at));
    writer.key_integer("total_bytes", static_cast<int64_t>(heap.total_bytes));
    writer.key_integer("object_count", static_cast<int64_t>(heap.object_count));
    writer.key_integer("alloc_count", static_cast<int64_t>(heap.alloc_count));
    writer.key_integer("free_count", static_cast<int64_t>(heap.free_count));
    writer.key_integer("unknown_frees",
                       static_cast<int64_t>(heap.unknown_frees));
    writer.key_integer("peak_bytes", static_cast<int64_t>(heap.peak_bytes));
    writer.key_integer("peak_ts", static_cast<int64_t>(heap.peak_ts));
    writer.key_integer("objects_truncated",
                       static_cast<int64_t>(heap.objects_truncated));
    writer.key_integer("sites_truncated",
                       static_cast<int64_t>(heap.sites_truncated));
    writer.key("sites");
    writer.begin_array();
    for (const auto& site : heap.sites) {
      writer.begin_object();
      writer.key_string("site", site.site);
      writer.key_string("class", site.klass);
      writer.key_integer("live_bytes", static_cast<int64_t>(site.live_bytes));
      writer.key_integer("live_count", static_cast<int64_t>(site.live_count));
      writer.key_integer("alloc_bytes", static_cast<int64_t>(site.alloc_bytes));
      writer.key_integer("alloc_count", static_cast<int64_t>(site.alloc_count));
      writer.key_integer("free_count", static_cast<int64_t>(site.free_count));
      writer.key_number("avg_age_ns", site.average_age_ns);
      writer.end_object();
    }
    writer.end_array();
    writer.key("objects");
    writer.begin_array();
    for (const auto& object : heap.objects) {
      writer.begin_object();
      writer.key_string("ptr", hex_pointer(object.ptr));
      writer.key_integer("size", static_cast<int64_t>(object.size));
      writer.key_string("owner", hex_pointer(object.owner));
      writer.key_integer("alloc_ts", static_cast<int64_t>(object.alloc_ts));
      writer.key_integer("age_ns", static_cast<int64_t>(object.age_ns));
      writer.key_integer("event_id", static_cast<int64_t>(object.alloc_record));
      writer.key_string("site", object.site);
      writer.key_string("class", object.klass);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/heap/graph", [get](const HttpRequest& request,
                               HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint64_t at = param_at(request, *trace);
    HeapGraphOptions options;
    options.max_nodes = static_cast<size_t>(
        parse_u64(request.query_value("nodes"), 4000, 50000));
    options.max_edges = 100000;
    const HeapGraph graph = build_heap_graph(*trace, at, options);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("at", static_cast<int64_t>(at));
    writer.key_boolean("converged", graph.converged);
    writer.key_boolean("truncated", graph.truncated);
    writer.key_integer("roots", static_cast<int64_t>(graph.roots));
    writer.key_integer("total_bytes", static_cast<int64_t>(graph.total_bytes));
    writer.key("nodes");
    writer.begin_array();
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
      const auto& node = graph.nodes[i];
      writer.begin_object();
      writer.key_integer("index", static_cast<int64_t>(i));
      writer.key_string("ptr", hex_pointer(node.ptr));
      writer.key_integer("size", static_cast<int64_t>(node.size));
      writer.key_integer("retained", static_cast<int64_t>(node.retained));
      writer.key_string("owner", hex_pointer(node.owner));
      writer.key_string("site", node.site);
      writer.key_string("class", node.klass);
      writer.key_integer("idom", node.idom);
      writer.key("children");
      writer.begin_array();
      for (uint32_t child : node.children) writer.integer(child);
      writer.end_array();
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/leaks", [get](const HttpRequest& request,
                          HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const size_t limit = static_cast<size_t>(
        parse_u64(request.query_value("limit"), 50, 500));
    const auto leaks = leak_candidates(*trace, limit);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("end_ts", static_cast<int64_t>(trace->end_ts()));
    writer.key_integer("count", static_cast<int64_t>(leaks.size()));
    writer.key("leaks");
    writer.begin_array();
    for (const auto& leak : leaks) {
      writer.begin_object();
      writer.key_string("site", leak.site);
      writer.key_string("class", leak.klass);
      writer.key_integer("bytes", static_cast<int64_t>(leak.bytes));
      writer.key_integer("count", static_cast<int64_t>(leak.count));
      writer.key_integer("oldest_ts", static_cast<int64_t>(leak.oldest_ts));
      writer.key_integer("newest_ts", static_cast<int64_t>(leak.newest_ts));
      writer.key_number("score", leak.score);
      writer.key_string("age", format_ns(trace->end_ts() - leak.oldest_ts));
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/flame", [get](const HttpRequest& request,
                          HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const TimeWindow window = window_of(request, *trace);
    const bool spans_mode = request.query_value("mode") == "spans";
    FlameOptions options;
    options.max_nodes = static_cast<size_t>(
        parse_u64(request.query_value("nodes"), 12000, kMaxFlameNodes));
    const FlameGraph graph = spans_mode
                                 ? build_span_flame(*trace, window, options)
                                 : build_flame(*trace, window, options);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("from", static_cast<int64_t>(graph.window.begin));
    writer.key_integer("to", static_cast<int64_t>(graph.window.end));
    writer.key_boolean("derived_from_spans", graph.derived_from_spans);
    writer.key_boolean("truncated", graph.truncated);
    writer.key_integer("samples", static_cast<int64_t>(graph.samples));
    writer.key_integer("total_weight",
                       static_cast<int64_t>(graph.total_weight));
    writer.key("root");
    size_t remaining = options.max_nodes;
    bool truncated_by_writer = false;
    write_flame_node(writer, graph.root, remaining, truncated_by_writer);
    writer.key_boolean("writer_truncated", truncated_by_writer);
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/state", [get](const HttpRequest& request,
                          HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint64_t at = param_at(request, *trace);
    const auto state = state_at(*trace, at);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("at", static_cast<int64_t>(at));
    writer.key_integer("count", static_cast<int64_t>(state.size()));
    writer.key("entries");
    writer.begin_array();
    for (const auto& entry : state) {
      writer.begin_object();
      writer.key_string("key", entry.key);
      if (entry.type == ValueType::String) {
        writer.key_string("type", "string");
        writer.key_string("text", entry.text);
      } else {
        writer.key_string("type", "number");
        writer.key_number("number", entry.number);
      }
      writer.key_integer("ts", static_cast<int64_t>(entry.ts));
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/state/history", [get](const HttpRequest& request,
                                     HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const auto key = request.query_value("key");
    if (key.empty() || key.size() > 512) {
      response.set_error(400, "a state key is required");
      return;
    }
    const size_t limit = static_cast<size_t>(
        parse_u64(request.query_value("limit"), 256, 2000));
    const uint64_t end = trace->end_ts();
    const uint64_t from =
        std::min(parse_u64(request.query_value("from"), 0, end), end);
    const uint64_t to =
        std::min(parse_u64(request.query_value("to"), end, end), end);
    const auto points = state_history(*trace, key, limit, from, to);
    json::Writer writer;
    writer.begin_object();
    writer.key_string("key", key);
    writer.key_integer("count", static_cast<int64_t>(points.size()));
    writer.key("points");
    writer.begin_array();
    for (const auto& point : points) {
      writer.begin_object();
      writer.key_integer("ts", static_cast<int64_t>(point.ts));
      if (point.type == ValueType::String) {
        writer.key_string("type", "string");
        writer.key_string("text", point.text);
      } else {
        writer.key_string("type", "number");
        writer.key_number("number", point.number);
      }
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/counters", [get](const HttpRequest& request,
                             HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const auto filter = request.query_value("name");
    const size_t max_points = static_cast<size_t>(
        parse_u64(request.query_value("points"), 512, 2048));
    const auto series = counter_series(*trace, filter, 32, max_points);
    json::Writer writer;
    writer.begin_object();
    writer.key("series");
    writer.begin_array();
    for (const auto& item : series) {
      writer.begin_object();
      writer.key_string("name", item.name);
      writer.key_string("unit", item.unit);
      writer.key_string("kind", to_string(item.kind));
      writer.key_number("min", item.minimum);
      writer.key_number("max", item.maximum);
      writer.key_number("last", item.last);
      writer.key("points");
      writer.begin_array();
      for (const auto& point : item.points) {
        writer.begin_array();
        writer.integer(static_cast<int64_t>(point.ts));
        writer.number(point.value);
        writer.end_array();
      }
      writer.end_array();
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });

  add("/api/export", [get, getter](const HttpRequest& request,
                                      HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint64_t at = param_at(request, *trace);
    const size_t limit = static_cast<size_t>(
        parse_u64(request.query_value("limit"), 500, 20000));
    HeapOptions heap_options;
    heap_options.max_objects = limit;
    heap_options.max_sites = 256;
    const HeapAt heap = heap_at(*trace, at, heap_options);
    const auto state = state_at(*trace, at);
    const auto spans = active_spans(*trace, at);
    const auto leaks = leak_candidates(*trace, 100);
    const TraceSnapshot snapshot = getter();
    json::Writer writer;
    writer.begin_object();
    writer.key_string("format", "glassbox-moment-1");
    writer.key_string("glassbox_version", GBX_VERSION);
    writer.key_integer("at", static_cast<int64_t>(at));
    writer.key_integer("wall_ns",
                       static_cast<int64_t>(trace->meta.wall_start_ns + at));
    writer.key_integer("duration_ns", static_cast<int64_t>(trace->end_ts()));
    writer.key_string("trace_name", snapshot.name);
    writer.key_string("trace_path", snapshot.path);
    writer.key_string("app", trace->meta.app_name);
    writer.key_string("arch", trace->meta.arch);
    writer.key("heap");
    writer.begin_object();
    writer.key_integer("total_bytes", static_cast<int64_t>(heap.total_bytes));
    writer.key_integer("object_count", static_cast<int64_t>(heap.object_count));
    writer.key_integer("peak_bytes", static_cast<int64_t>(heap.peak_bytes));
    writer.key_integer("alloc_count", static_cast<int64_t>(heap.alloc_count));
    writer.key_integer("free_count", static_cast<int64_t>(heap.free_count));
    writer.key("sites");
    writer.begin_array();
    for (const auto& site : heap.sites) {
      writer.begin_object();
      writer.key_string("site", site.site);
      writer.key_string("class", site.klass);
      writer.key_integer("live_bytes", static_cast<int64_t>(site.live_bytes));
      writer.key_integer("live_count", static_cast<int64_t>(site.live_count));
      writer.key_number("avg_age_ns", site.average_age_ns);
      writer.end_object();
    }
    writer.end_array();
    writer.key("objects");
    writer.begin_array();
    for (const auto& object : heap.objects) {
      writer.begin_object();
      writer.key_string("ptr", hex_pointer(object.ptr));
      writer.key_integer("size", static_cast<int64_t>(object.size));
      writer.key_string("owner", hex_pointer(object.owner));
      writer.key_integer("alloc_ts", static_cast<int64_t>(object.alloc_ts));
      writer.key_integer("age_ns", static_cast<int64_t>(object.age_ns));
      writer.key_string("site", object.site);
      writer.key_string("class", object.klass);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    writer.key("state");
    writer.begin_array();
    for (const auto& entry : state) {
      writer.begin_object();
      writer.key_string("key", entry.key);
      if (entry.type == ValueType::String) {
        writer.key_string("text", entry.text);
      } else {
        writer.key_number("number", entry.number);
      }
      writer.key_integer("ts", static_cast<int64_t>(entry.ts));
      writer.end_object();
    }
    writer.end_array();
    writer.key("active_spans");
    writer.begin_array();
    for (const auto& span : spans) {
      writer.begin_object();
      writer.key_integer("id", static_cast<int64_t>(span.id));
      writer.key_integer("parent", static_cast<int64_t>(span.parent));
      writer.key_integer("thread", static_cast<int64_t>(span.thread));
      writer.key_string("name", span.name);
      writer.key_string("kind", span.kind);
      writer.key_integer("begin_ts", static_cast<int64_t>(span.begin_ts));
      writer.end_object();
    }
    writer.end_array();
    writer.key("leaks");
    writer.begin_array();
    for (const auto& leak : leaks) {
      writer.begin_object();
      writer.key_string("site", leak.site);
      writer.key_string("class", leak.klass);
      writer.key_integer("bytes", static_cast<int64_t>(leak.bytes));
      writer.key_integer("count", static_cast<int64_t>(leak.count));
      writer.key_number("score", leak.score);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
        response.content_type = "application/json; charset=utf-8";
    response.headers.emplace_back(
        "Content-Disposition",
        "attachment; filename=\"glassbox-moment.json\"");
  });

  add("/api/diff", [get](const HttpRequest& request,
                         HttpResponse& response) {
    auto trace = get(request, response);
    if (!trace) return;
    const uint64_t end_limit = trace->end_ts();
    const uint64_t a = parse_u64(request.query_value("a"), 0, end_limit);
    const uint64_t b =
        parse_u64(request.query_value("b"), end_limit, end_limit);
    const size_t limit = static_cast<size_t>(
        parse_u64(request.query_value("limit"), 100, 500));
    const DiffResult delta = diff(*trace, a, b, limit);
    json::Writer writer;
    writer.begin_object();
    writer.key_integer("a", static_cast<int64_t>(delta.a));
    writer.key_integer("b", static_cast<int64_t>(delta.b));
    writer.key_integer("total_bytes_a",
                       static_cast<int64_t>(delta.total_bytes_a));
    writer.key_integer("total_bytes_b",
                       static_cast<int64_t>(delta.total_bytes_b));
    writer.key_integer("total_bytes_delta", delta.total_bytes_delta);
    writer.key_integer("objects_a", static_cast<int64_t>(delta.objects_a));
    writer.key_integer("objects_b", static_cast<int64_t>(delta.objects_b));
    writer.key_integer("objects_delta", delta.objects_delta);
    writer.key("heap");
    writer.begin_array();
    for (const auto& item : delta.heap) {
      writer.begin_object();
      writer.key_string("site", item.site);
      writer.key_string("class", item.klass);
      writer.key_integer("bytes_a", static_cast<int64_t>(item.bytes_a));
      writer.key_integer("bytes_b", static_cast<int64_t>(item.bytes_b));
      writer.key_integer("bytes_delta", item.bytes_delta);
      writer.key_integer("count_a", static_cast<int64_t>(item.count_a));
      writer.key_integer("count_b", static_cast<int64_t>(item.count_b));
      writer.key_integer("count_delta", item.count_delta);
      writer.end_object();
    }
    writer.end_array();
    writer.key("spans");
    writer.begin_array();
    for (const auto& item : delta.spans) {
      writer.begin_object();
      writer.key_string("name", item.name);
      writer.key_integer("count_a", static_cast<int64_t>(item.count_a));
      writer.key_integer("count_b", static_cast<int64_t>(item.count_b));
      writer.key_integer("count_delta", item.count_delta);
      writer.key_integer("total_ns_a", static_cast<int64_t>(item.total_ns_a));
      writer.key_integer("total_ns_b", static_cast<int64_t>(item.total_ns_b));
      writer.key_integer("total_ns_delta", item.total_ns_delta);
      writer.end_object();
    }
    writer.end_array();
    writer.key("counters");
    writer.begin_array();
    for (const auto& item : delta.counters) {
      writer.begin_object();
      writer.key_string("name", item.name);
      writer.key_number("before", item.before);
      writer.key_number("after", item.after);
      writer.key_number("delta", item.delta);
      writer.key_boolean("present_before", item.present_before);
      writer.key_boolean("present_after", item.present_after);
      writer.end_object();
    }
    writer.end_array();
    writer.key("logs");
    writer.begin_array();
    for (const auto& item : delta.logs) {
      writer.begin_object();
      writer.key_string("level", to_string(item.level));
      writer.key_integer("count_a", static_cast<int64_t>(item.count_a));
      writer.key_integer("count_b", static_cast<int64_t>(item.count_b));
      writer.key_integer("delta", item.delta);
      writer.end_object();
    }
    writer.end_array();
    writer.key("state");
    writer.begin_array();
    for (const auto& item : delta.state) {
      writer.begin_object();
      writer.key_string("key", item.key);
      writer.key_string("before", item.before);
      writer.key_string("after", item.after);
      writer.key_boolean("changed", item.changed);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });
}

}  // namespace gbx::ui
