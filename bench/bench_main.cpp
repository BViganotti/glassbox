// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Benchmark suite. Every number printed here is measured at runtime on the
// machine that runs it; nothing is hard-coded.
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "glassbox/analysis.hpp"
#include "glassbox/json.hpp"
#include "glassbox/query.hpp"
#include "glassbox/recorder.hpp"
#include "glassbox/trace_io.hpp"

namespace {

using Clock = std::chrono::steady_clock;

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

double median(std::vector<double> values) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-bench-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

void line(const char* label, const std::string& value) {
  std::printf("%-26s %s\n", label, value.c_str());
}

std::string human_rate(uint64_t count, double seconds) {
  const double rate = seconds > 0 ? static_cast<double>(count) / seconds : 0;
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "%.0f ops/s", rate);
  return buffer;
}

struct BuiltTrace {
  gbx::Trace trace;
  uint64_t bytes = 0;
};

BuiltTrace build_trace(uint32_t scale) {
  BuiltTrace built;
  gbx::Trace& trace = built.trace;
  trace.meta.app_name = "benchmark";
  trace.meta.app_version = "1";
  trace.meta.wall_start_ns = 1700000000000000000ull;
  auto str = [&trace](std::string_view text) {
    return trace.strings.try_intern(text).value_or(0);
  };
  std::vector<gbx::Field> fields;
  auto emit = [&](uint64_t ts, gbx::RecordType type) {
    gbx::Record record;
    record.ts = ts;
    record.type = type;
    record.thread = static_cast<uint32_t>(ts % 4) + 1;
    trace.add_record(record, fields);
    fields.clear();
  };

  const uint32_t spans = 6000 * scale;
  for (uint32_t i = 0; i < spans; ++i) {
    const uint64_t begin = 1000 + static_cast<uint64_t>(i) * 40;
    const uint32_t id = i + 1;
    fields.push_back({str("id"), gbx::Value::integer(id)});
    fields.push_back({str("parent"), gbx::Value::integer(i % 3 == 0 ? 0 : (i / 3) + 1)});
    fields.push_back({str("name"), gbx::Value::str(str(i % 2 ? "Update" : "Render"))});
    fields.push_back({str("kind"), gbx::Value::str(str("cpu"))});
    emit(begin, gbx::RecordType::SpanBegin);
    fields.push_back({str("id"), gbx::Value::integer(id)});
    emit(begin + 35, gbx::RecordType::SpanEnd);
  }

  const uint32_t allocs = 40000 * scale;
  for (uint32_t i = 0; i < allocs; ++i) {
    const uint64_t ts = 2000 + static_cast<uint64_t>(i) * 6;
    const uint64_t ptr = 0x100000 + static_cast<uint64_t>(i) * 64;
    fields.push_back({str("ptr"), gbx::Value::integer(static_cast<int64_t>(ptr))});
    fields.push_back({str("size"), gbx::Value::integer(64 + (i % 4096))});
    fields.push_back({str("site"), gbx::Value::str(str("alloc.cpp:42"))});
    fields.push_back({str("class"), gbx::Value::str(str(i % 3 ? "Mesh" : "Texture"))});
    emit(ts, gbx::RecordType::Alloc);
    if (i % 3 == 0) {
      fields.push_back({str("ptr"), gbx::Value::integer(static_cast<int64_t>(ptr))});
      emit(ts + 2, gbx::RecordType::Free);
    }
  }

  const uint32_t logs = 30000 * scale;
  for (uint32_t i = 0; i < logs; ++i) {
    fields.push_back({str("level"), gbx::Value::integer(i % 6)});
    fields.push_back({str("msg"), gbx::Value::str(str("frame checkpoint"))});
    emit(3000 + static_cast<uint64_t>(i) * 9, gbx::RecordType::Log);
  }

  const uint32_t samples = 8000 * scale;
  for (uint32_t i = 0; i < samples; ++i) {
    fields.push_back({str("frames"),
                      gbx::Value::str(str("all\x1fmain\x1fUpdate\x1fPhysics"))});
    fields.push_back({str("weight"), gbx::Value::real(1.0)});
    emit(4000 + static_cast<uint64_t>(i) * 7, gbx::RecordType::Sample);
  }

  for (uint32_t i = 0; i < 2000 * scale; ++i) {
    fields.push_back({str("name"), gbx::Value::str(str("fps"))});
    fields.push_back({str("value"), gbx::Value::real(60.0)});
    emit(5000 + static_cast<uint64_t>(i) * 10, gbx::RecordType::Counter);
  }
  trace.finalize();
  return built;
}

void bench_recorder(uint32_t operations) {
  const std::string path = temp_path("recorder.gbt");
  gbx::RecorderConfig config;
  config.ring_capacity = 65536;
  gbx::Recorder recorder(config);
  if (!recorder.start(path).ok()) {
    std::printf("recorder: FAILED to start\n");
    return;
  }
  const uint64_t start = now_ns();
  for (uint32_t i = 0; i < operations; ++i) {
    recorder.log(gbx::LogLevel::Info, "benchmark log line", "bench");
    recorder.counter("fps", 60.0);
    const auto ptr = reinterpret_cast<void*>(0x1000000ull + i * 64);
    recorder.alloc(ptr, 64, "bench.cpp:1", "Bench");
    recorder.free(ptr);
  }
  const auto stats = recorder.stats();
  const uint64_t mid = now_ns();
  const auto finish = recorder.finish();
  const uint64_t end = now_ns();
  if (!finish.ok()) {
    std::printf("recorder: finish failed: %s\n", finish.describe().c_str());
    return;
  }
  const double seconds = static_cast<double>(mid - start) / 1e9;
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer),
                "%llu records in %.3fs · %s · %.1f MB/s · dropped=%llu",
                static_cast<unsigned long long>(stats.records_written), seconds,
                human_rate(stats.records_written, seconds).c_str(),
                static_cast<double>(stats.bytes_written) / 1e6 / seconds,
                static_cast<unsigned long long>(stats.records_dropped));
  line("Recorder append", buffer);
  std::snprintf(buffer, sizeof(buffer), "%.3fs",
                static_cast<double>(end - mid) / 1e9);
  line("Recorder finish", buffer);
}

void bench_trace_io(uint32_t scale) {
  const auto built = build_trace(scale);
  const std::string path = temp_path("analysis.gbt");
  gbx::SaveOptions options;
  options.chunk_bytes = 1u << 20;
  const uint64_t start = now_ns();
  auto status = gbx::save_trace(path, built.trace, options);
  const uint64_t end = now_ns();
  if (!status.ok()) {
    std::printf("writer: %s\n", status.describe().c_str());
    return;
  }
  const auto bytes = std::filesystem::file_size(path);
  line("Trace writer",
       std::to_string(built.trace.size()) + " records in " +
           std::to_string(static_cast<double>(end - start) / 1e6) + "ms · " +
           std::to_string(bytes / 1024) + " KiB · " +
           human_rate(built.trace.size(), static_cast<double>(end - start) / 1e9));

  std::vector<double> load_ms;
  gbx::Trace loaded;
  for (int i = 0; i < 5; ++i) {
    const uint64_t begin = now_ns();
    status = gbx::load_trace(path, loaded);
    const uint64_t close = now_ns();
    if (!status.ok()) {
      std::printf("loader: %s\n", status.describe().c_str());
      return;
    }
    load_ms.push_back(static_cast<double>(close - begin) / 1e6);
  }
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "%.1f ms median · %s · %zu records",
                median(load_ms),
                human_rate(loaded.size(), median(load_ms) / 1000).c_str(),
                loaded.size());
  line("Trace reader", buffer);
  std::snprintf(buffer, sizeof(buffer), "%zu KiB in memory",
                loaded.memory_usage() / 1024);
  line("Trace memory", buffer);

  const uint64_t end_ts = loaded.end_ts();
  std::vector<double> timeline_ms;
  std::vector<double> heap_ms;
  std::vector<double> flame_ms;
  std::vector<double> diff_ms;
  std::vector<double> query_ms;
  for (int i = 0; i < 5; ++i) {
    uint64_t begin = now_ns();
    gbx::build_timeline(loaded, 200);
    timeline_ms.push_back(static_cast<double>(now_ns() - begin) / 1e6);

    begin = now_ns();
    gbx::HeapOptions heap_options;
    heap_options.max_objects = 500;
    heap_options.max_sites = 128;
    gbx::heap_at(loaded, end_ts / 2, heap_options);
    heap_ms.push_back(static_cast<double>(now_ns() - begin) / 1e6);

    begin = now_ns();
    gbx::build_flame(loaded, {});
    flame_ms.push_back(static_cast<double>(now_ns() - begin) / 1e6);

    begin = now_ns();
    gbx::diff(loaded, 0, end_ts, 100);
    diff_ms.push_back(static_cast<double>(now_ns() - begin) / 1e6);

    begin = now_ns();
    gbx::QueryError error;
    auto query = gbx::Query::parse("type=alloc AND size>1024", &error);
    if (query.valid()) gbx::run_query(loaded, query, 0, end_ts, 500);
    query_ms.push_back(static_cast<double>(now_ns() - begin) / 1e6);
  }
  std::snprintf(buffer, sizeof(buffer), "%.1f ms median", median(timeline_ms));
  line("Timeline (200 buckets)", buffer);
  std::snprintf(buffer, sizeof(buffer), "%.1f ms median @ T/2", median(heap_ms));
  line("Heap reconstruction", buffer);
  std::snprintf(buffer, sizeof(buffer), "%.1f ms median", median(flame_ms));
  line("Flame aggregation", buffer);
  std::snprintf(buffer, sizeof(buffer), "%.1f ms median", median(diff_ms));
  line("Moment diff", buffer);
  std::snprintf(buffer, sizeof(buffer), "%.1f ms median (limit 500)",
                median(query_ms));
  line("Query scan", buffer);
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) quick = true;
  }
  struct utsname info {};
  ::uname(&info);
  std::printf("glassbox benchmark · %s %s · %ld cpus\n", info.machine,
              info.release, sysconf(_SC_NPROCESSORS_ONLN));
  std::printf("--------------------------------------------------------------\n");
  bench_recorder(quick ? 25000 : 100000);
  bench_trace_io(quick ? 1 : 2);
  std::printf("--------------------------------------------------------------\n");
  std::printf("Values are measured live; file paths live under the temp dir.\n");
  return 0;
}
