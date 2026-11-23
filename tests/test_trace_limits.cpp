// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "glassbox/trace_io.hpp"
#include "test_framework.hpp"
#include "trace/crc32.hpp"
#include "trace/file_io.hpp"
#include "trace/format.hpp"
#include "trace/varint.hpp"

namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
  }
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
  }
}

std::vector<uint8_t> file_header() {
  std::vector<uint8_t> out;
  out.insert(out.end(), gbx::trace::format::kMagic.begin(),
             gbx::trace::format::kMagic.end());
  put_u32(out, gbx::trace::format::kVersion);
  put_u32(out, 0);
  put_u64(out, 0);
  put_u32(out, 0);
  put_u32(out, 0);  // no metadata strings
  return out;
}

std::vector<uint8_t> chunk_with_payload(const std::vector<uint8_t>& payload,
                                        uint32_t records = 1,
                                        uint32_t declared_crc = 0,
                                        bool override_crc = false) {
  std::vector<uint8_t> out;
  out.insert(out.end(), gbx::trace::format::kChunkMagic.begin(),
             gbx::trace::format::kChunkMagic.end());
  put_u32(out, static_cast<uint32_t>(payload.size()));
  put_u32(out, override_crc ? declared_crc : gbx::trace::crc32(payload));
  put_u32(out, records);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

gbx::Status load(const std::vector<uint8_t>& bytes, gbx::Trace& trace,
                 gbx::LoadOptions options = {}) {
  return gbx::load_trace_bytes(bytes, trace, options, "limit-test.gbt");
}

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-limits-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

}  // namespace

TEST(trace_limits_oversized_chunk_payload) {
  auto bytes = file_header();
  bytes.insert(bytes.end(), gbx::trace::format::kChunkMagic.begin(),
               gbx::trace::format::kChunkMagic.end());
  put_u32(bytes, gbx::trace::format::kMaxChunkPayload + 1);
  put_u32(bytes, 0);
  put_u32(bytes, 0);
  gbx::Trace trace;
  const auto status = load(bytes, trace);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
}

TEST(trace_limits_chunk_record_count_must_match) {
  std::vector<uint8_t> payload;
  gbx::trace::ByteWriter writer(payload);
  writer.varint(0);  // strings
  writer.varint(1);  // records
  auto bytes = file_header();
  const auto chunk = chunk_with_payload(payload, 2);  // says 2, payload has 1
  bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  gbx::Trace trace;
  const auto status = load(bytes, trace);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
}

TEST(trace_limits_unknown_value_type) {
  std::vector<uint8_t> payload;
  gbx::trace::ByteWriter writer(payload);
  writer.varint(1);
  writer.varint(1);  // string "k"
  writer.raw("k", 1);
  writer.varint(1);  // one record
  writer.varint(0);  // ts
  writer.u8(static_cast<uint8_t>(gbx::RecordType::Custom));
  writer.varint(0);  // thread
  writer.varint(0);  // span
  writer.varint(1);  // one field
  writer.varint(0);  // key
  writer.u8(9);      // invalid value type
  auto bytes = file_header();
  const auto chunk = chunk_with_payload(payload);
  bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  gbx::Trace trace;
  const auto status = load(bytes, trace);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
}

TEST(trace_limits_record_budget) {
  gbx::Trace source;
  for (int i = 0; i < 10; ++i) {
    gbx::Record record;
    record.ts = static_cast<uint64_t>(i);
    record.type = gbx::RecordType::Marker;
    CHECK(source.add_record(record, {}).ok());
  }
  source.finalize();
  const std::string path = temp_path("records.gbt");
  CHECK(gbx::save_trace(path, source).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 20, bytes).ok());
  gbx::LoadOptions options;
  options.limits.max_records = 5;
  gbx::Trace trace;
  const auto status = load(bytes, trace, options);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
}

TEST(trace_limits_string_budget) {
  gbx::Trace source;
  std::vector<gbx::Field> fields;
  for (int i = 0; i < 6; ++i) {
    const auto key = source.strings.try_intern("key-" + std::to_string(i));
    const auto value = source.strings.try_intern("value-" + std::to_string(i));
    CHECK(key.has_value());
    CHECK(value.has_value());
    fields.push_back({*key, gbx::Value::str(*value)});
  }
  gbx::Record record;
  record.type = gbx::RecordType::Marker;
  CHECK(source.add_record(record, fields).ok());
  source.finalize();
  const std::string path = temp_path("strings.gbt");
  CHECK(gbx::save_trace(path, source).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 20, bytes).ok());
  gbx::LoadOptions options;
  options.limits.max_strings = 3;
  gbx::Trace trace;
  const auto status = load(bytes, trace, options);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
}

TEST(trace_limits_chunk_budget) {
  gbx::Trace source;
  for (int i = 0; i < 10; ++i) {
    gbx::Record record;
    record.ts = static_cast<uint64_t>(i);
    record.type = gbx::RecordType::Marker;
    CHECK(source.add_record(record, {}).ok());
  }
  source.finalize();
  const std::string path = temp_path("chunks.gbt");
  gbx::SaveOptions save;
  save.chunk_records = 1;
  CHECK(gbx::save_trace(path, source, save).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 20, bytes).ok());
  gbx::LoadOptions options;
  options.limits.max_chunks = 2;
  gbx::Trace trace;
  const auto status = load(bytes, trace, options);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
}

TEST(trace_limits_chunk_index_uses_min_max) {
  gbx::Trace source;
  for (uint64_t ts : {uint64_t{100}, uint64_t{50}}) {
    gbx::Record record;
    record.ts = ts;
    record.type = gbx::RecordType::Marker;
    CHECK(source.add_record(record, {}).ok());
  }
  const std::string path = temp_path("unordered.gbt");
  CHECK(gbx::save_trace(path, source).ok());
  gbx::Trace loaded;
  CHECK(gbx::load_trace(path, loaded).ok());
  CHECK_EQ(loaded.chunks.size(), size_t{1});
  CHECK_EQ(loaded.chunks[0].first_ts, uint64_t{50});
  CHECK_EQ(loaded.chunks[0].last_ts, uint64_t{100});
  CHECK_EQ(loaded.records[0].ts, uint64_t{50});
}

TEST(trace_limits_footer_mismatch) {
  gbx::Trace source;
  gbx::Record record;
  record.type = gbx::RecordType::Marker;
  CHECK(source.add_record(record, {}).ok());
  source.finalize();
  const std::string path = temp_path("footer.gbt");
  CHECK(gbx::save_trace(path, source).ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 20, bytes).ok());
  CHECK_GT(bytes.size(), size_t{40});
  // Footer layout: magic(8) chunk_count(8) record_count(8) duration(8) crc(4).
  const size_t tail = bytes.size() - 36;
  CHECK_EQ(std::memcmp(bytes.data() + tail, "GBXEND01", 8), 0);
  bytes[tail + 8 + 8] = 9;  // corrupt the record count
  const uint32_t crc = gbx::trace::crc32(
      std::span<const uint8_t>(bytes.data() + tail + 8, 24));
  for (int i = 0; i < 4; ++i) {
    bytes[tail + 32 + static_cast<size_t>(i)] =
        static_cast<uint8_t>((crc >> (i * 8)) & 0xff);
  }
  gbx::Trace trace;
  const auto status = load(bytes, trace);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
}

TEST(trace_limits_timestamp_underflow) {
  std::vector<uint8_t> payload;
  gbx::trace::ByteWriter writer(payload);
  writer.varint(0);
  writer.varint(2);
  // Record 1 at ts=10.
  writer.varint(10);
  writer.u8(static_cast<uint8_t>(gbx::RecordType::Marker));
  writer.varint(0);
  writer.varint(0);
  writer.varint(0);
  // Record 2 with a negative delta larger than 10.
  writer.svarint(-20);
  writer.u8(static_cast<uint8_t>(gbx::RecordType::Marker));
  writer.varint(0);
  writer.varint(0);
  writer.varint(0);
  auto bytes = file_header();
  const auto chunk = chunk_with_payload(payload, 2);
  bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  gbx::Trace trace;
  const auto status = load(bytes, trace);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
}
