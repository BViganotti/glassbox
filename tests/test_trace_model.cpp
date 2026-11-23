// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/trace.hpp"

#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

struct Builder {
  gbx::Trace trace;
  std::vector<gbx::Field> scratch;

  uint32_t str(std::string_view s) {
    auto id = trace.strings.try_intern(s);
    CHECK(id.has_value());
    return id.value_or(0);
  }

  const gbx::Field& add(std::string_view key, gbx::Value value) {
    scratch.push_back({str(key), value});
    return scratch.back();
  }

  void emit(uint64_t ts, gbx::RecordType type, uint32_t thread = 0,
            uint32_t span = 0) {
    gbx::Record record;
    record.ts = ts;
    record.type = type;
    record.thread = thread;
    record.span = span;
    CHECK(trace.add_record(record, scratch).ok());
    scratch.clear();
  }
};

}  // namespace

TEST(trace_add_and_sort) {
  Builder b;
  b.add(gbx::keys::kMsg, gbx::Value::str(b.str("second")));
  b.emit(20, gbx::RecordType::Log);
  b.add(gbx::keys::kMsg, gbx::Value::str(b.str("first")));
  b.emit(10, gbx::RecordType::Log);
  b.add(gbx::keys::kMsg, gbx::Value::str(b.str("equal-a")));
  b.emit(20, gbx::RecordType::Log);
  b.trace.finalize();
  CHECK_EQ(b.trace.size(), size_t{3});
  CHECK_EQ(b.trace.end_ts(), uint64_t{20});
  CHECK_EQ(b.trace.str_field(b.trace.records[0], gbx::keys::kMsg),
           std::string_view("first"));
  CHECK_EQ(b.trace.str_field(b.trace.records[1], gbx::keys::kMsg),
           std::string_view("second"));
  CHECK_EQ(b.trace.str_field(b.trace.records[2], gbx::keys::kMsg),
           std::string_view("equal-a"));  // stable for equal timestamps
}

TEST(trace_fields_resolve) {
  Builder b;
  b.add(gbx::keys::kSize, gbx::Value::integer(4096));
  b.add(gbx::keys::kSite, gbx::Value::str(b.str("alloc.cpp:42")));
  b.emit(5, gbx::RecordType::Alloc);
  b.trace.finalize();
  const auto& rec = b.trace.records[0];
  CHECK_EQ(b.trace.int_field(rec, gbx::keys::kSize), int64_t{4096});
  CHECK_EQ(b.trace.str_field(rec, gbx::keys::kSite),
           std::string_view("alloc.cpp:42"));
  CHECK_EQ(b.trace.int_field(rec, "missing", -1), int64_t{-1});
  CHECK(b.trace.field(rec, "missing") == nullptr);
}

TEST(trace_thread_names) {
  Builder b;
  b.add(gbx::keys::kName, gbx::Value::str(b.str("render")));
  b.emit(1, gbx::RecordType::ThreadName, 2);
  b.add(gbx::keys::kMsg, gbx::Value::str(b.str("hi")));
  b.emit(2, gbx::RecordType::Log, 2);
  b.trace.finalize();
  CHECK_EQ(b.trace.thread_name(2), std::string_view("render"));
  CHECK_EQ(b.trace.thread_name(0), std::string_view(""));
  CHECK_EQ(b.trace.thread_count(), size_t{3});
}

TEST(trace_limits_reject_oversized_record) {
  gbx::TraceLimits limits;
  limits.max_record_fields = 2;
  gbx::Trace trace(limits);
  std::vector<gbx::Field> fields(3, {0, gbx::Value::integer(1)});
  CHECK(!trace.add_record({}, fields).ok());
}

TEST(trace_limits_reject_record_overflow) {
  gbx::TraceLimits limits;
  limits.max_records = 2;
  gbx::Trace trace(limits);
  CHECK(trace.add_record(gbx::Record{}, {}).ok());
  CHECK(trace.add_record(gbx::Record{}, {}).ok());
  auto status = trace.add_record(gbx::Record{}, {});
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
}

TEST(trace_record_range) {
  Builder b;
  for (uint64_t ts : {uint64_t{0}, uint64_t{10}, uint64_t{20}, uint64_t{30}}) {
    b.emit(ts, gbx::RecordType::Marker);
  }
  b.trace.finalize();
  auto [first, last] = b.trace.record_range(10, 20);
  CHECK_EQ(first, size_t{1});
  CHECK_EQ(last, size_t{3});
  auto empty = b.trace.record_range(21, 29);
  CHECK_EQ(empty.first, size_t{0});
  CHECK_EQ(empty.second, size_t{0});
  auto all = b.trace.record_range(0, 100);
  CHECK_EQ(all.first, size_t{0});
  CHECK_EQ(all.second, size_t{4});
}

TEST(trace_snapshot_view) {
  Builder b;
  const uint32_t kHealth = b.str("health");
  const uint32_t kMode = b.str("mode");
  b.add("health", gbx::Value::real(87.5));
  b.add("mode", gbx::Value::str(b.str("combat")));
  b.emit(7, gbx::RecordType::Snapshot);
  b.trace.finalize();
  gbx::SnapshotView view(b.trace, b.trace.records[0]);
  CHECK_EQ(view.size(), size_t{2});
  CHECK_EQ(view.key_at(0), std::string_view("health"));
  CHECK_NEAR(view.value_at(0).as_double(), 87.5, 1e-9);
  CHECK_EQ(view.string_at(1).value_or(""), std::string_view("combat"));
  CHECK(!view.string_at(0).has_value());
  CHECK_EQ(view.key_at(99), std::string_view(""));
  (void)kHealth;
  (void)kMode;
}
