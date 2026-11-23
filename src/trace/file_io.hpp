// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "glassbox/status.hpp"

namespace gbx::trace {

/// Thin owned file descriptor wrapper with checked writes. All operations are
/// non-throwing and report failures as Status.
class FileWriter {
 public:
  FileWriter() = default;
  ~FileWriter();
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;
  FileWriter(FileWriter&& other) noexcept;
  FileWriter& operator=(FileWriter&& other) noexcept;

  /// Creates/truncates `path`, creating parent directories when `make_dirs`.
  Status open(const std::string& path, bool make_dirs = true);
  Status write(const void* data, size_t size);
  Status write(std::span<const uint8_t> data) {
    return write(data.data(), data.size());
  }
  /// Writes a little-endian uint32 (trace headers are explicitly encoded).
  Status write_u32(uint32_t value);
  Status write_u64(uint64_t value);
  Status flush();
  Status close();
  Status sync();

  bool is_open() const { return fd_ >= 0; }
  uint64_t offset() const { return offset_; }
  const std::string& path() const { return path_; }
  const Status& error() const { return error_; }

 private:
  int fd_ = -1;
  uint64_t offset_ = 0;
  std::string path_;
  Status error_;
};

/// Sequential reader with an explicit size budget. It never allocates more
/// than the caller asks for, so the trace loader can stream chunk by chunk.
class FileReader {
 public:
  FileReader() = default;
  ~FileReader();
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  Status open(const std::string& path, uint64_t max_bytes);
  Status read_exact(void* out, size_t size);
  Status read_exact(std::vector<uint8_t>& out, size_t size);
  Status close();

  bool is_open() const { return fd_ >= 0; }
  uint64_t size() const { return size_; }
  uint64_t offset() const { return offset_; }
  uint64_t remaining() const { return offset_ <= size_ ? size_ - offset_ : 0; }
  const std::string& path() const { return path_; }

 private:
  int fd_ = -1;
  uint64_t size_ = 0;
  uint64_t offset_ = 0;
  std::string path_;
};

/// Reads a whole regular file into memory, refusing files larger than
/// `max_bytes` before allocating.
Status read_file(const std::string& path, size_t max_bytes,
                 std::vector<uint8_t>& out);

/// Returns true when the path exists and is a regular file.
bool is_regular_file(const std::string& path);

/// Creates missing parent directories for `path`.
Status ensure_parent_dirs(const std::string& path);

}  // namespace gbx::trace
