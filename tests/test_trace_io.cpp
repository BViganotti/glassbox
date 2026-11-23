// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/trace_io.hpp"

#include "trace/file_io.hpp"

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

std::string temp_file(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-trace-io-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

struct Fixture {
  gbx::Trace trace;

  uint32_t str(std::string_view s) {
    auto id = trace.strings.try_intern(s);
    CHECK(id.has_value());
    return id.value_or(0);
  }

  void add(gbx::RecordType type, uint64_t ts, uint32_t thread,
           std::vector<gbx::Field> fields) {
    gbx::Record record;
    record.ts = ts;
    record.thread = thread;
    record.type = type;
    CHECK(trace.add_record(record, fields).ok());
  }

  void populate() {
    trace.meta.app_name = "unit-test";
    trace.meta.app_version = "1.2.3";
    trace.meta.exe_path = "/tmp/app";
    trace.meta.arch = "arm64";
    trace.meta.os = "Darwin";
    trace.meta.wall_start_ns = 1700000000000000000ull;
    trace.meta.pid = 4242;

    add(gbx::RecordType::ThreadName, 0, 1,
        {{str("name"), gbx::Value::str(str("main"))}});
    for (int i = 0; i < 10; ++i) {
      std::vector<gbx::Field> fields;
      fields.push_back({str("name"), gbx::Value::str(str("Frame"))});
      fields.push_back({str("kind"), gbx::Value::str(str("cpu"))});
      fields.push_back({str("id"), gbx::Value::integer(i + 1)});
      add(gbx::RecordType::SpanBegin, static_cast<uint64_t>(i) * 100, 1,
          fields);
      add(gbx::RecordType::Log, static_cast<uint64_t>(i) * 100 + 10, 1,
          {{str("level"), gbx::Value::integer(2)},
           {str("msg"), gbx::Value::str(str("frame started"))},
           {str("duration"), gbx::Value::real(12.5)},
           {str("ok"), gbx::Value::boolean(true)}});
      add(gbx::RecordType::Alloc, static_cast<uint64_t>(i) * 100 + 20, 1,
          {{str("ptr"), gbx::Value::integer(0x1000 + i)},
           {str("size"), gbx::Value::integer(64 * (i + 1))},
           {str("site"), gbx::Value::str(str("render.cpp:31"))},
           {str("class"), gbx::Value::str(str("Mesh"))}});
      add(gbx::RecordType::Free, static_cast<uint64_t>(i) * 100 + 30, 1,
          {{str("ptr"), gbx::Value::integer(0x1000 + i)}});
    }
    add(gbx::RecordType::Snapshot, 1000, 1,
        {{str("health"), gbx::Value::real(90.0)},
         {str("mode"), gbx::Value::str(str("idle"))}});
    add(gbx::RecordType::Sample, 1010, 1,
        {{str("frames"), gbx::Value::str(str("main\x1f" "Update\x1f" "Physics"))},
         {str("weight"), gbx::Value::real(1.0)}});
    add(gbx::RecordType::Marker, 1020, 0,
        {{str("label"), gbx::Value::str(str("end"))}});
    trace.finalize();
  }
};

}  // namespace

TEST(trace_io_roundtrip) {
  Fixture fixture;
  fixture.populate();
  const std::string path = temp_file("roundtrip.gbt");
  auto status = gbx::save_trace(path, fixture.trace);
  CHECK_MSG(status.ok(), status.describe());

  gbx::Trace loaded;
  status = gbx::load_trace(path, loaded);
  CHECK_MSG(status.ok(), status.describe());
  CHECK_EQ(loaded.size(), fixture.trace.size());
  CHECK_EQ(loaded.meta.app_name, std::string("unit-test"));
  CHECK_EQ(loaded.meta.pid, uint32_t{4242});
  CHECK_EQ(loaded.meta.wall_start_ns, 1700000000000000000ull);
  CHECK_EQ(loaded.end_ts(), fixture.trace.end_ts());
  CHECK(!loaded.diagnostics.truncated);
  CHECK(!loaded.diagnostics.recovered);
  CHECK(!loaded.chunks.empty());
  CHECK_EQ(loaded.diagnostics.bytes_read,
           static_cast<size_t>(std::filesystem::file_size(path)));
  CHECK_NEAR(loaded.meta.recorded_ns, 1020.0, 0.5);

  // Records come back sorted with identical field payloads.
  const gbx::Record* first_span = nullptr;
  const gbx::Record* first_alloc = nullptr;
  const gbx::Record* sample = nullptr;
  for (const auto& r : loaded.records) {
    if (!first_span && r.type == gbx::RecordType::SpanBegin) first_span = &r;
    if (!first_alloc && r.type == gbx::RecordType::Alloc) first_alloc = &r;
    if (r.type == gbx::RecordType::Sample) sample = &r;
  }
  CHECK(first_span != nullptr);
  CHECK(first_alloc != nullptr);
  CHECK(sample != nullptr);
  if (first_span) {
    CHECK_EQ(loaded.str_field(*first_span, "name"), std::string_view("Frame"));
  }
  if (first_alloc) {
    CHECK_EQ(loaded.int_field(*first_alloc, "size"), int64_t{64});
    CHECK_EQ(loaded.str_field(*first_alloc, "class"), std::string_view("Mesh"));
  }
  CHECK(sample != nullptr);
  if (sample) {
    CHECK_EQ(loaded.str_field(*sample, "frames"),
             std::string_view("main\x1f" "Update\x1f" "Physics"));
  }
}

TEST(trace_io_multiple_chunks) {
  Fixture fixture;
  fixture.populate();
  gbx::SaveOptions options;
  options.chunk_records = 3;
  options.chunk_bytes = 128;
  const std::string path = temp_file("chunks.gbt");
  auto status = gbx::save_trace(path, fixture.trace, options);
  CHECK(status.ok());
  gbx::Trace loaded;
  status = gbx::load_trace(path, loaded);
  CHECK(status.ok());
  CHECK_EQ(loaded.size(), fixture.trace.size());
  CHECK_GT(loaded.chunks.size(), size_t{2});
  CHECK_EQ(loaded.chunks.front().first_ts, uint64_t{0});
  CHECK_EQ(loaded.chunks.back().last_ts, uint64_t{1020});
  // Each chunk starts at a chunk header recorded in the index and the offsets
  // are strictly increasing.
  for (size_t i = 1; i < loaded.chunks.size(); ++i) {
    CHECK_GT(loaded.chunks[i].file_offset, loaded.chunks[i - 1].file_offset);
  }
}

TEST(trace_io_empty_trace) {
  gbx::Trace trace;
  trace.meta.app_name = "empty";
  const std::string path = temp_file("empty.gbt");
  CHECK(gbx::save_trace(path, trace).ok());
  gbx::Trace loaded;
  auto status = gbx::load_trace(path, loaded);
  CHECK(status.ok());
  CHECK(loaded.empty());
  CHECK_EQ(loaded.end_ts(), uint64_t{0});
  CHECK_EQ(loaded.chunks.size(), size_t{0});
}

TEST(trace_io_rejects_bad_magic) {
  const std::string path = temp_file("bad-magic.gbt");
  {
    gbx::trace::FileWriter writer;
    CHECK(writer.open(path).ok());
    const std::string junk(100, 'x');
    CHECK(writer.write(junk.data(), junk.size()).ok());
    CHECK(writer.close().ok());
  }
  gbx::Trace loaded;
  auto status = gbx::load_trace(path, loaded);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
}

TEST(trace_io_detects_corruption) {
  Fixture fixture;
  fixture.populate();
  const std::string path = temp_file("corrupt.gbt");
  CHECK(gbx::save_trace(path, fixture.trace).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 24, bytes).ok());
  // Flip a byte inside the first chunk payload (after its 20-byte header).
  size_t chunk_at = 0;
  for (size_t i = 0; i + 8 <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, "GBXCHNK1", 8) == 0) {
      chunk_at = i;
      break;
    }
  }
  CHECK_GT(chunk_at, size_t{0});
  CHECK_GT(bytes.size(), chunk_at + 24);
  bytes[chunk_at + 24] ^= 0x20;
  gbx::Trace loaded;
  auto status =
      gbx::load_trace_bytes(bytes, loaded, {}, "corrupt.gbt");
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
}

TEST(trace_io_recovers_truncated_file) {
  Fixture fixture;
  fixture.populate();
  const std::string path = temp_file("truncated.gbt");
  CHECK(gbx::save_trace(path, fixture.trace).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 24, bytes).ok());
  CHECK_GT(bytes.size(), size_t{60});
  bytes.resize(bytes.size() - 10);  // cut into the footer

  gbx::Trace loaded;
  auto status = gbx::load_trace_bytes(bytes, loaded, {}, "cut.gbt");
  CHECK_MSG(status.ok(), status.describe());
  CHECK(loaded.diagnostics.truncated);
  CHECK(loaded.diagnostics.recovered);
  CHECK(!loaded.diagnostics.warning.empty());
  CHECK_EQ(loaded.size(), fixture.trace.size());

  gbx::LoadOptions strict;
  strict.recover_truncated = false;
  gbx::Trace strict_loaded;
  status = gbx::load_trace_bytes(bytes, strict_loaded, strict, "cut.gbt");
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Truncated);
}

TEST(trace_io_rejects_truncated_chunk_payload) {
  Fixture fixture;
  fixture.populate();
  const std::string path = temp_file("cut-chunk.gbt");
  CHECK(gbx::save_trace(path, fixture.trace).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 24, bytes).ok());
  // Keep the header, cut inside the first chunk payload.
  bytes.resize(100);
  gbx::Trace loaded;
  auto status = gbx::load_trace_bytes(bytes, loaded, {}, "cut.gbt");
  CHECK(status.ok());  // recovered an empty prefix
  CHECK(loaded.diagnostics.truncated);
  CHECK(loaded.empty());

  gbx::LoadOptions strict;
  strict.recover_truncated = false;
  gbx::Trace strict_loaded;
  status = gbx::load_trace_bytes(bytes, strict_loaded, strict, "cut.gbt");
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Truncated);
}

TEST(trace_io_rejects_unknown_version) {
  const std::string path = temp_file("version.gbt");
  {
    gbx::trace::FileWriter writer;
    CHECK(writer.open(path).ok());
    CHECK(writer.write("GLASSBOX", 8).ok());
    CHECK(writer.write_u32(99).ok());
    CHECK(writer.write_u32(0).ok());
    CHECK(writer.write_u64(0).ok());
    CHECK(writer.write_u32(0).ok());
    CHECK(writer.write_u32(0).ok());
    CHECK(writer.close().ok());
  }
  gbx::Trace loaded;
  auto status = gbx::load_trace(path, loaded);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Unsupported);
}

TEST(trace_io_rejects_trailing_garbage) {
  Fixture fixture;
  fixture.populate();
  const std::string path = temp_file("garbage.gbt");
  CHECK(gbx::save_trace(path, fixture.trace).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 24, bytes).ok());
  bytes.push_back(0x42);
  gbx::Trace loaded;
  auto status = gbx::load_trace_bytes(bytes, loaded, {}, "garbage.gbt");
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
}

TEST(trace_io_empty_file) {
  const std::string path = temp_file("zero.gbt");
  {
    gbx::trace::FileWriter writer;
    CHECK(writer.open(path).ok());
    CHECK(writer.close().ok());
  }
  gbx::Trace loaded;
  auto status = gbx::load_trace(path, loaded);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Truncated);
}
