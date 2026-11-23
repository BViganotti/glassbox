// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/recorder.hpp"

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "glassbox/span.hpp"
#include "glassbox/trace_io.hpp"
#include "trace/file_io.hpp"
#include "test_framework.hpp"

namespace {

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-recorder-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  std::filesystem::remove(dir / name);
  return (dir / name).string();
}

size_t count_type(const gbx::Trace& trace, gbx::RecordType type) {
  size_t n = 0;
  for (const auto& record : trace.records) {
    if (record.type == type) ++n;
  }
  return n;
}

const gbx::Record* find_type(const gbx::Trace& trace, gbx::RecordType type) {
  for (const auto& record : trace.records) {
    if (record.type == type) return &record;
  }
  return nullptr;
}

}  // namespace

TEST(recorder_basic_roundtrip) {
  const std::string path = temp_path("basic.gbt");
  gbx::RecorderConfig config;
  config.ring_capacity = 4096;
  gbx::Recorder rec(config);
  CHECK(!rec.is_recording());
  CHECK(rec.start(path).ok());
  CHECK(rec.is_recording());
  CHECK_EQ(rec.thread_slot() > 0, true);

  const uint32_t outer = rec.span_begin("Frame", "cpu");
  CHECK_EQ(rec.current_span(), outer);
  {
    const uint32_t inner = rec.span_begin("Physics", "cpu");
    CHECK_EQ(rec.current_span(), inner);
    rec.log(gbx::LogLevel::Info, "inner work", "physics");
    rec.span_end(inner);
  }
  CHECK_EQ(rec.current_span(), outer);
  rec.counter("fps", 59.5, "hz");
  rec.gauge("heap", 1024.0);
  rec.mark("checkpoint");
  rec.alloc(reinterpret_cast<const void*>(0x1234), 256, "demo.cpp:10", "Mesh");
  rec.free(reinterpret_cast<const void*>(0x1234));
  rec.snapshot({{"health", 87.5}, {"entities", 12}});
  rec.snapshot_text({{"mode", "combat"}});
  const std::string_view frames[] = {"main", "Update", "Physics"};
  rec.sample(frames, 2.0);
  rec.span_end(outer);
  CHECK_EQ(rec.current_span(), uint32_t{0});

  auto status = rec.finish();
  CHECK_MSG(status.ok(), status.describe());
  CHECK(!rec.is_recording());
  const auto stats = rec.stats();
  CHECK_GT(stats.records_written, uint64_t{0});
  CHECK_EQ(stats.records_dropped, uint64_t{0});
  CHECK_EQ(stats.spans_started, uint64_t{2});
  CHECK_EQ(stats.spans_ended, uint64_t{2});
  CHECK_GT(stats.bytes_written, uint64_t{100});
  CHECK_GT(stats.chunks, uint64_t{0});

  gbx::Trace trace;
  status = gbx::load_trace(path, trace);
  CHECK_MSG(status.ok(), status.describe());
  CHECK(!trace.diagnostics.truncated);
  CHECK_EQ(count_type(trace, gbx::RecordType::SpanBegin), size_t{2});
  CHECK_EQ(count_type(trace, gbx::RecordType::SpanEnd), size_t{2});
  CHECK_EQ(count_type(trace, gbx::RecordType::Log), size_t{1});
  CHECK_EQ(count_type(trace, gbx::RecordType::Alloc), size_t{1});
  CHECK_EQ(count_type(trace, gbx::RecordType::Free), size_t{1});
  CHECK_EQ(count_type(trace, gbx::RecordType::Snapshot), size_t{2});
  CHECK_EQ(count_type(trace, gbx::RecordType::Sample), size_t{1});
  CHECK_EQ(count_type(trace, gbx::RecordType::Counter), size_t{1});
  CHECK_EQ(count_type(trace, gbx::RecordType::Gauge), size_t{1});

  const auto* begin = find_type(trace, gbx::RecordType::SpanBegin);
  CHECK(begin != nullptr);
  if (begin) CHECK_EQ(trace.str_field(*begin, "name"), std::string_view("Frame"));
  const auto* alloc = find_type(trace, gbx::RecordType::Alloc);
  CHECK(alloc != nullptr);
  if (alloc) {
    CHECK_EQ(trace.int_field(*alloc, "size"), int64_t{256});
    CHECK_EQ(trace.str_field(*alloc, "class"), std::string_view("Mesh"));
  }
  const auto* sample = find_type(trace, gbx::RecordType::Sample);
  CHECK(sample != nullptr);
  if (sample) {
    CHECK_EQ(trace.str_field(*sample, "frames"),
             std::string_view("main\x1f" "Update\x1f" "Physics"));
  }
  // Nested log records carry the innermost span id.
  const auto* log = find_type(trace, gbx::RecordType::Log);
  CHECK(log != nullptr);
  uint32_t physics_id = 0;
  for (const auto& record : trace.records) {
    if (record.type == gbx::RecordType::SpanBegin &&
        trace.str_field(record, "name") == "Physics") {
      physics_id = static_cast<uint32_t>(trace.int_field(record, "id"));
    }
  }
  CHECK_GT(physics_id, uint32_t{0});
  if (log) CHECK_EQ(log->span, physics_id);
}

TEST(recorder_raii_span) {
  const std::string path = temp_path("raii.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  {
    auto span = rec.begin_span("Scoped", "test");
    CHECK(span.active());
    CHECK(rec.current_span() == span.id());
    span.log(gbx::LogLevel::Debug, "inside");
  }
  CHECK_EQ(rec.current_span(), uint32_t{0});
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  CHECK_EQ(count_type(trace, gbx::RecordType::SpanBegin), size_t{1});
  CHECK_EQ(count_type(trace, gbx::RecordType::SpanEnd), size_t{1});
}

TEST(recorder_rejects_after_finish) {
  const std::string path = temp_path("after.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  rec.log(gbx::LogLevel::Info, "before");
  CHECK(rec.finish().ok());
  const auto before = rec.stats();
  rec.log(gbx::LogLevel::Info, "after");
  const auto after = rec.stats();
  CHECK_EQ(after.records_rejected, before.records_rejected + 1);
  CHECK(rec.finish().ok());  // idempotent
  CHECK(!rec.start(path).ok());  // cannot restart
}

TEST(recorder_multithreaded_smoke) {
  const std::string path = temp_path("threads.gbt");
  gbx::RecorderConfig config;
  config.ring_capacity = 65536;
  gbx::Recorder rec(config);
  CHECK(rec.start(path).ok());
  constexpr int kThreads = 4;
  constexpr int kPerThread = 500;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&rec, t] {
      rec.thread_name("worker-" + std::to_string(t));
      for (int i = 0; i < kPerThread; ++i) {
        auto span = rec.begin_span("Work", "cpu");
        rec.log(gbx::LogLevel::Info, "tick", "worker");
        rec.counter("ticks", 1.0);
        (void)span;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  auto status = rec.finish();
  CHECK_MSG(status.ok(), status.describe());
  const auto stats = rec.stats();
  CHECK_EQ(stats.records_dropped, uint64_t{0});
  gbx::Trace trace;
  status = gbx::load_trace(path, trace);
  CHECK(status.ok());
  CHECK_EQ(count_type(trace, gbx::RecordType::Log),
           static_cast<size_t>(kThreads * kPerThread));
  CHECK_EQ(count_type(trace, gbx::RecordType::SpanBegin),
           static_cast<size_t>(kThreads * kPerThread));
  CHECK_EQ(count_type(trace, gbx::RecordType::SpanEnd),
           static_cast<size_t>(kThreads * kPerThread));
  // Timestamps are sorted on load and thread names survived.
  for (size_t i = 1; i < trace.records.size(); ++i) {
    CHECK(trace.records[i - 1].ts <= trace.records[i].ts);
  }
  CHECK_GT(trace.thread_count(), size_t{1});
  bool found_worker = false;
  for (uint32_t i = 0; i < trace.thread_count(); ++i) {
    if (trace.thread_name(i).rfind("worker-", 0) == 0) found_worker = true;
  }
  CHECK(found_worker);
}

TEST(recorder_live_sink) {
  const std::string path = temp_path("live.gbt");
  gbx::Recorder rec;
  std::mutex mu;
  std::vector<gbx::LiveRecord> live;
  rec.set_live_sink([&](const gbx::LiveRecord& record) {
    std::lock_guard lock(mu);
    live.push_back(record);
  });
  CHECK(rec.start(path).ok());
  {
    auto span = rec.begin_span("Live", "test");
    rec.log(gbx::LogLevel::Warn, "streaming", "net");
    rec.alloc(reinterpret_cast<const void*>(0x2000), 32, "x.cpp:1", "Buf");
  }
  CHECK(rec.finish().ok());
  std::lock_guard lock(mu);
  CHECK_GT(live.size(), size_t{3});
  bool found_log = false;
  for (const auto& record : live) {
    if (record.type != gbx::RecordType::Log) continue;
    found_log = true;
    CHECK_EQ(record.fields.size(), size_t{3});
    CHECK_EQ(record.fields[0].key, std::string("level"));
    CHECK_EQ(record.fields[1].text, std::string("streaming"));
    CHECK_EQ(record.fields[2].text, std::string("net"));
    CHECK(!record.thread_name.empty());
  }
  CHECK(found_log);
}

TEST(recorder_string_limit_accounts_drops) {
  const std::string path = temp_path("limits.gbt");
  gbx::RecorderConfig config;
  config.max_strings = 32;   // enough for keys, not for 200 unique messages
  config.max_string_bytes = 4096;
  gbx::Recorder rec(config);
  CHECK(rec.start(path).ok());
  for (int i = 0; i < 200; ++i) {
    rec.log(gbx::LogLevel::Info, "unique-message-" + std::to_string(i));
  }
  CHECK(rec.finish().ok());
  const auto stats = rec.stats();
  CHECK_GT(stats.records_dropped, uint64_t{0});
  CHECK_GT(stats.strings_dropped, uint64_t{0});

  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  bool found_warning = false;
  for (const auto& record : trace.records) {
    if (record.type != gbx::RecordType::Marker) continue;
    if (trace.str_field(record, "name") == "recorder_warnings" ||
        trace.field(record, "dropped_records") != nullptr) {
      found_warning = true;
      CHECK_GT(trace.int_field(record, "dropped_records"), int64_t{0});
    }
  }
  CHECK(found_warning);
}

TEST(recorder_cancel_leaves_truncated_trace) {
  const std::string path = temp_path("cancel.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  for (int i = 0; i < 10; ++i) rec.log(gbx::LogLevel::Info, "x");
  rec.cancel();
  CHECK(!rec.is_recording());
  CHECK(gbx::trace::is_regular_file(path));
  gbx::Trace trace;
  auto status = gbx::load_trace(path, trace);
  CHECK(status.ok());
  CHECK(trace.diagnostics.truncated);
  CHECK(trace.diagnostics.recovered);
}

TEST(recorder_start_requires_unique_use) {
  gbx::Recorder rec;
  const std::string path = temp_path("start-twice.gbt");
  CHECK(rec.start(path).ok());
  auto second = rec.start(path + ".2");
  CHECK(!second.ok());
  CHECK(second.code() == gbx::ErrorCode::Busy);
  CHECK(rec.finish().ok());
}
