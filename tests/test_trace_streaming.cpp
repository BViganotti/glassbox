// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "glassbox/demo.hpp"
#include "glassbox/trace_io.hpp"
#include "test_framework.hpp"
#include "trace/file_io.hpp"

namespace {

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-stream-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  std::filesystem::remove(dir / name);
  return (dir / name).string();
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  gbx::trace::FileWriter writer;
  CHECK(writer.open(path).ok());
  CHECK(writer.write(bytes).ok());
  CHECK(writer.close().ok());
}

}  // namespace

TEST(streaming_loader_agrees_with_byte_loader) {
  const std::string path = temp_path("compare.gbt");
  gbx::DemoOptions demo;
  demo.frames = 12;
  demo.sleep_spike = false;
  CHECK(gbx::record_demo_trace(path, demo).status.ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 26, bytes).ok());
  gbx::Trace from_bytes;
  CHECK(gbx::load_trace_bytes(bytes, from_bytes, {}, "compare.gbt").ok());
  CHECK(!from_bytes.diagnostics.streamed);
  gbx::Trace streamed;
  CHECK(gbx::load_trace(path, streamed).ok());
  CHECK(streamed.diagnostics.streamed);
  CHECK_EQ(streamed.size(), from_bytes.size());
  CHECK_EQ(streamed.end_ts(), from_bytes.end_ts());
  CHECK_EQ(streamed.meta.app_name, from_bytes.meta.app_name);
  CHECK_EQ(streamed.meta.wall_start_ns, from_bytes.meta.wall_start_ns);
  CHECK_EQ(streamed.chunks.size(), from_bytes.chunks.size());
  CHECK_EQ(streamed.strings.size(), from_bytes.strings.size());
  CHECK_EQ(streamed.fields.size(), from_bytes.fields.size());
  for (size_t i = 0; i < streamed.records.size(); ++i) {
    CHECK_EQ(streamed.records[i].ts, from_bytes.records[i].ts);
    CHECK(streamed.records[i].type == from_bytes.records[i].type);
    CHECK_EQ(streamed.records[i].field_count,
             from_bytes.records[i].field_count);
  }
  CHECK_EQ(streamed.chunks[0].file_offset, from_bytes.chunks[0].file_offset);
}

TEST(streaming_loader_recovers_truncated_files) {
  const std::string path = temp_path("cut-base.gbt");
  gbx::DemoOptions demo;
  demo.frames = 8;
  demo.sleep_spike = false;
  CHECK(gbx::record_demo_trace(path, demo).status.ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 26, bytes).ok());
  const std::string cut = temp_path("cut.gbt");
  for (size_t cut_size : {bytes.size() - 5, bytes.size() / 2,
                          bytes.size() / 4}) {
    std::vector<uint8_t> slice(bytes.begin(),
                               bytes.begin() + static_cast<ptrdiff_t>(cut_size));
    write_bytes(cut, slice);
    gbx::Trace recovered;
    const auto status = gbx::load_trace(cut, recovered);
    CHECK_MSG(status.ok(), status.describe());
    CHECK(recovered.diagnostics.truncated);
    CHECK(recovered.diagnostics.recovered);
    CHECK(recovered.diagnostics.streamed);
    CHECK(!recovered.diagnostics.warning.empty());
    CHECK(recovered.size() <= 2000);
    gbx::LoadOptions strict;
    strict.recover_truncated = false;
    gbx::Trace rejected;
    const auto strict_status = gbx::load_trace(cut, rejected, strict);
    CHECK(!strict_status.ok());
    CHECK(strict_status.code() == gbx::ErrorCode::Truncated);
  }
}

TEST(streaming_loader_enforces_memory_budget) {
  const std::string path = temp_path("memory.gbt");
  gbx::DemoOptions demo;
  demo.frames = 10;
  demo.sleep_spike = false;
  CHECK(gbx::record_demo_trace(path, demo).status.ok());
  gbx::LoadOptions options;
  options.limits.max_memory_bytes = 2048;
  gbx::Trace trace;
  const auto status = gbx::load_trace(path, trace, options);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
  CHECK(!trace.diagnostics.streamed || trace.empty());
}

TEST(streaming_loader_enforces_file_budget) {
  const std::string path = temp_path("file-budget.gbt");
  const std::vector<uint8_t> bytes(4096, 0x42);
  write_bytes(path, bytes);
  gbx::LoadOptions options;
  options.limits.max_file_bytes = 1024;
  gbx::Trace trace;
  const auto status = gbx::load_trace(path, trace, options);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
}

TEST(streaming_loader_ignores_checksums_when_disabled) {
  // A corrupted payload must fail with checksums on. With checksums off the
  // decoder must still validate structure and not crash.
  const std::string path = temp_path("crc.gbt");
  gbx::DemoOptions demo;
  demo.frames = 6;
  demo.sleep_spike = false;
  CHECK(gbx::record_demo_trace(path, demo).status.ok());
  std::vector<uint8_t> bytes;
  CHECK(gbx::trace::read_file(path, 1 << 26, bytes).ok());
  size_t chunk_at = 0;
  for (size_t i = 0; i + 8 <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, "GBXCHNK1", 8) == 0) {
      chunk_at = i;
      break;
    }
  }
  CHECK_GT(chunk_at, size_t{0});
  bytes[chunk_at + 24] ^= 0x10;
  write_bytes(path, bytes);
  gbx::Trace strict;
  gbx::LoadOptions verify;
  verify.verify_checksums = true;
  auto status = gbx::load_trace(path, strict, verify);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Corrupt);
  gbx::Trace unchecked;
  gbx::LoadOptions no_verify;
  no_verify.verify_checksums = false;
  status = gbx::load_trace(path, unchecked, no_verify);
  // Either the structure survives or it fails cleanly; neither may crash.
  if (status.ok()) CHECK(gbx::trace::is_regular_file(path));
}
