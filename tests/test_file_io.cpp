// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "trace/file_io.hpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

std::string temp_file(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

}  // namespace

TEST(file_io_roundtrip) {
  const std::string path = temp_file("io.bin");
  const std::vector<uint8_t> data{1, 2, 3, 0, 255, 42};
  {
    gbx::trace::FileWriter writer;
    CHECK(writer.open(path).ok());
    CHECK(writer.write(data).ok());
    CHECK_EQ(writer.offset(), static_cast<uint64_t>(data.size()));
    CHECK(writer.close().ok());
  }
  std::vector<uint8_t> read;
  CHECK(gbx::trace::read_file(path, 1 << 20, read).ok());
  CHECK(read == data);
}

TEST(file_io_header_encoding) {
  const std::string path = temp_file("header.bin");
  gbx::trace::FileWriter writer;
  CHECK(writer.open(path).ok());
  CHECK(writer.write_u32(0x01020304u).ok());
  CHECK(writer.write_u64(0x1122334455667788ull).ok());
  CHECK(writer.close().ok());
  std::vector<uint8_t> read;
  CHECK(gbx::trace::read_file(path, 1 << 20, read).ok());
  CHECK_EQ(read.size(), size_t{12});
  CHECK_EQ(read[0], uint8_t{0x04});
  CHECK_EQ(read[4], uint8_t{0x88});
  CHECK_EQ(read[11], uint8_t{0x11});
}

TEST(file_io_refuses_oversized_read) {
  const std::string path = temp_file("big.bin");
  gbx::trace::FileWriter writer;
  CHECK(writer.open(path).ok());
  const std::vector<uint8_t> data(1024, 7);
  CHECK(writer.write(data).ok());
  CHECK(writer.close().ok());
  std::vector<uint8_t> read;
  auto status = gbx::trace::read_file(path, 100, read);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::LimitExceeded);
  CHECK(read.empty());
}

TEST(file_io_missing_file) {
  std::vector<uint8_t> read;
  auto status = gbx::trace::read_file("/nonexistent/glassbox/file", 100, read);
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::IoError);
  CHECK(!gbx::trace::is_regular_file("/nonexistent/glassbox/file"));
}

TEST(file_io_creates_parents) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-test-dirs-" + std::to_string(::getpid()));
  const std::string path = (dir / "a" / "b" / "c.bin").string();
  std::filesystem::remove_all(dir);
  gbx::trace::FileWriter writer;
  CHECK(writer.open(path).ok());
  CHECK(writer.write_u32(1).ok());
  CHECK(writer.close().ok());
  CHECK(gbx::trace::is_regular_file(path));
  std::filesystem::remove_all(dir);
}

TEST(file_reader_incremental_and_limits) {
  const std::string path = temp_file("reader.bin");
  const std::vector<uint8_t> data{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  {
    gbx::trace::FileWriter writer;
    CHECK(writer.open(path).ok());
    CHECK(writer.write(data).ok());
    CHECK(writer.close().ok());
  }
  gbx::trace::FileReader reader;
  CHECK(reader.open(path, data.size()).ok());
  CHECK_EQ(reader.size(), static_cast<uint64_t>(data.size()));
  uint8_t first[4] = {};
  CHECK(reader.read_exact(first, sizeof(first)).ok());
  CHECK_EQ(first[3], uint8_t{4});
  CHECK_EQ(reader.remaining(), uint64_t{6});
  uint8_t too_much[7] = {};
  const auto status = reader.read_exact(too_much, sizeof(too_much));
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Truncated);
  CHECK(reader.close().ok());

  gbx::trace::FileReader limited;
  const auto limit_status = limited.open(path, 4);
  CHECK(!limit_status.ok());
  CHECK(limit_status.code() == gbx::ErrorCode::LimitExceeded);

  gbx::trace::FileReader directory;
  const auto directory_status = directory.open(
      std::filesystem::temp_directory_path().string(), 1 << 20);
  CHECK(!directory_status.ok());
}
