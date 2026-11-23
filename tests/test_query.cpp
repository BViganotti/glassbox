// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/query.hpp"

#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

struct Fixture {
  gbx::Trace trace;
  std::vector<gbx::Field> scratch;

  uint32_t str(std::string_view s) {
    auto id = trace.strings.try_intern(s);
    CHECK(id.has_value());
    return id.value_or(0);
  }

  void emit(uint64_t ts, gbx::RecordType type, uint32_t thread,
            uint32_t span = 0) {
    gbx::Record record;
    record.ts = ts;
    record.type = type;
    record.thread = thread;
    record.span = span;
    CHECK(trace.add_record(record, scratch).ok());
    scratch.clear();
  }

  void log(uint64_t ts, uint32_t thread, gbx::LogLevel level,
           std::string_view message) {
    scratch.push_back({str("level"), gbx::Value::integer(static_cast<int64_t>(level))});
    scratch.push_back({str("msg"), gbx::Value::str(str(message))});
    emit(ts, gbx::RecordType::Log, thread);
  }

  void alloc(uint64_t ts, uint32_t thread, uint64_t size, std::string_view klass,
             std::string_view site) {
    scratch.push_back({str("size"), gbx::Value::integer(static_cast<int64_t>(size))});
    scratch.push_back({str("class"), gbx::Value::str(str(klass))});
    scratch.push_back({str("site"), gbx::Value::str(str(site))});
    scratch.push_back({str("ptr"), gbx::Value::integer(static_cast<int64_t>(ts + 1))});
    emit(ts, gbx::RecordType::Alloc, thread);
  }

  void build() {
    log(0, 1, gbx::LogLevel::Info, "Engine boot");
    alloc(10, 1, 64, "Mesh", "render.cpp:10");
    log(20, 2, gbx::LogLevel::Error, "Texture upload failed");
    alloc(30, 2, 4096, "Texture", "texture.cpp:30");
    log(40, 1, gbx::LogLevel::Warn, "Slow frame detected");
    alloc(50, 1, 1024, "Buffer", "render.cpp:10");
    log(60, 2, gbx::LogLevel::Debug, "mesh cache warm");
    gbx::Record marker;
    marker.ts = 70;
    marker.type = gbx::RecordType::Marker;
    marker.thread = 1;
    scratch.push_back({str("label"), gbx::Value::str(str("checkpoint"))});
    CHECK(trace.add_record(marker, scratch).ok());
    scratch.clear();
    trace.finalize();
  }
};

size_t count_matches(const Fixture& f, std::string_view query) {
  gbx::QueryError error;
  auto parsed = gbx::Query::parse(query, &error);
  if (!parsed.valid()) return size_t{9999};
  size_t n = 0;
  for (const auto& record : f.trace.records) {
    if (parsed.matches(f.trace, record)) ++n;
  }
  return n;
}

}  // namespace

TEST(query_parse_and_type_filter) {
  Fixture f;
  f.build();
  CHECK_EQ(count_matches(f, "type = log"), size_t{4});
  CHECK_EQ(count_matches(f, "type:log"), size_t{4});
  CHECK_EQ(count_matches(f, "type = alloc"), size_t{3});
  CHECK_EQ(count_matches(f, "type = nope"), size_t{0});
}

TEST(query_numeric_comparisons) {
  Fixture f;
  f.build();
  CHECK_EQ(count_matches(f, "type = alloc AND size > 100"), size_t{2});
  CHECK_EQ(count_matches(f, "type = alloc AND size >= 1024"), size_t{2});
  CHECK_EQ(count_matches(f, "type = alloc AND size < 1024"), size_t{1});
  CHECK_EQ(count_matches(f, "thread = 2"), size_t{3});
  CHECK_EQ(count_matches(f, "ts >= 30 AND ts < 60"), size_t{3});
  CHECK_EQ(count_matches(f, "level >= 3"), size_t{2});
}

TEST(query_string_ops_and_text_search) {
  Fixture f;
  f.build();
  CHECK_EQ(count_matches(f, "msg ~ \"FRAME\""), size_t{1});
  CHECK_EQ(count_matches(f, "msg ~ slow"), size_t{1});
  CHECK_EQ(count_matches(f, "msg !~ slow"), size_t{3});
  CHECK_EQ(count_matches(f, "class = Texture"), size_t{1});
  CHECK_EQ(count_matches(f, "texture"), size_t{2});  // log message + alloc (once)
  CHECK_EQ(count_matches(f, "checkpoint"), size_t{1});
}

TEST(query_boolean_logic_and_precedence) {
  Fixture f;
  f.build();
  CHECK_EQ(count_matches(f, "type = log OR type = alloc AND size > 100"),
           size_t{6});
  CHECK_EQ(count_matches(f, "(type = log OR type = alloc) AND size > 100"),
           size_t{2});
  CHECK_EQ(count_matches(f, "NOT type = log"), size_t{4});
  CHECK_EQ(count_matches(f, "!(type = log)"), size_t{4});
  CHECK_EQ(count_matches(f, "type = log AND NOT level = 2"), size_t{3});
  CHECK_EQ(count_matches(f, "type = log && level = 4"), size_t{1});
  CHECK_EQ(count_matches(f, "type = log || type = marker"), size_t{5});
}

TEST(query_durations) {
  Fixture f;
  f.build();
  CHECK_EQ(count_matches(f, "ts >= 20ns"), size_t{6});
  CHECK_EQ(count_matches(f, "ts < 1ms"), size_t{8});
  CHECK_EQ(count_matches(f, "ts >= 0.00002ms"), size_t{6});
}

TEST(query_missing_fields) {
  Fixture f;
  f.build();
  CHECK_EQ(count_matches(f, "site = render.cpp:10"), size_t{2});
  CHECK_EQ(count_matches(f, "nosuchfield = 1"), size_t{0});
  CHECK_EQ(count_matches(f, "nosuchfield != 1"), size_t{0});
  CHECK_EQ(count_matches(f, "level > 0"), size_t{4});
  // A string value compared against a numeric field is a valid query that
  // simply does not match.
  CHECK_EQ(count_matches(f, "size > abc"), size_t{0});
}

TEST(query_errors) {
  const char* invalid[] = {
      "type = ",          "((type = log)",  "type = log)",
      "AND type = log",   "type ~",         "\"unterminated",
      "type = log extra", "ts > 1zz",
  };
  for (const char* text : invalid) {
    gbx::QueryError error;
    auto query = gbx::Query::parse(text, &error);
    CHECK_MSG(!query.valid(), text);
    if (!query.valid()) CHECK(!error.message.empty());
  }
  std::string deep;
  for (int i = 0; i < 40; ++i) deep += "(";
  deep += "type = log";
  for (int i = 0; i < 40; ++i) deep += ")";
  gbx::QueryError error;
  CHECK(!gbx::Query::parse(deep, &error).valid());
  CHECK_EQ(error.message, std::string("expression nesting too deep"));
  const std::string too_long(5000, 'a');
  CHECK(!gbx::Query::parse(too_long, &error).valid());
  CHECK(error.message.find("too long") != std::string::npos);
}

TEST(query_describe_and_counts) {
  gbx::QueryError error;
  auto query = gbx::Query::parse("type = log AND size >= 10", &error);
  CHECK(query.valid());
  CHECK_EQ(query.source(), std::string("type = log AND size >= 10"));
  CHECK(query.describe().find("AND") != std::string::npos);
  CHECK(query.describe().find("type=\"log\"") != std::string::npos);
  CHECK_EQ(query.node_count(), size_t{3});
}

TEST(query_run_query_limit_and_order) {
  Fixture f;
  f.build();
  auto query = gbx::Query::parse("type = log");
  CHECK(query.valid());
  auto all = gbx::run_query(f.trace, query, 0, 1000);
  CHECK_EQ(all.size(), size_t{4});
  auto limited = gbx::run_query(f.trace, query, 0, 1000, 2);
  CHECK_EQ(limited.size(), size_t{2});
  CHECK_EQ(f.trace.records[limited[0]].ts, uint64_t{0});
  auto newest = gbx::run_query(f.trace, query, 0, 1000, 2, true);
  CHECK_EQ(newest.size(), size_t{2});
  CHECK_EQ(f.trace.records[newest[0]].ts, uint64_t{60});
  auto window = gbx::run_query(f.trace, query, 20, 40);
  CHECK_EQ(window.size(), size_t{2});
  gbx::Query empty;
  CHECK(!empty.valid());
  CHECK(gbx::run_query(f.trace, empty, 0, 1000).empty());
}
