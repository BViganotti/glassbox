// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "trace/file_io.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <utility>

namespace gbx::trace {
namespace {

Status errno_status(std::string_view what) {
  return Status::error(ErrorCode::IoError,
                       std::string(what) + ": " + std::strerror(errno));
}

}  // namespace

FileWriter::~FileWriter() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

FileWriter::FileWriter(FileWriter&& other) noexcept
    : fd_(other.fd_),
      offset_(other.offset_),
      path_(std::move(other.path_)),
      error_(std::move(other.error_)) {
  other.fd_ = -1;
  other.offset_ = 0;
}

FileWriter& FileWriter::operator=(FileWriter&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    offset_ = other.offset_;
    path_ = std::move(other.path_);
    error_ = std::move(other.error_);
    other.fd_ = -1;
    other.offset_ = 0;
  }
  return *this;
}

Status FileWriter::open(const std::string& path, bool make_dirs) {
  if (fd_ >= 0) {
    return Status::error(ErrorCode::Busy, "file writer already open");
  }
  if (make_dirs) {
    auto status = ensure_parent_dirs(path);
    if (!status.ok()) return status;
  }
  path_ = path;
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) return errno_status("open " + path);
  offset_ = 0;
  error_ = Status::success();
  return Status::success();
}

Status FileWriter::write(const void* data, size_t size) {
  if (!error_.ok()) return error_;
  if (fd_ < 0) {
    return Status::error(ErrorCode::IoError, "write on closed file");
  }
  if (size > 0 && data == nullptr) {
    return Status::error(ErrorCode::InvalidArgument, "null write buffer");
  }
  const auto* p = static_cast<const uint8_t*>(data);
  size_t left = size;
  while (left > 0) {
    const ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      error_ = errno_status("write " + path_);
      return error_;
    }
    if (n == 0) {
      error_ = Status::error(ErrorCode::IoError, "short write to " + path_);
      return error_;
    }
    p += static_cast<size_t>(n);
    left -= static_cast<size_t>(n);
    offset_ += static_cast<uint64_t>(n);
  }
  return Status::success();
}

Status FileWriter::write_u32(uint32_t value) {
  uint8_t bytes[4];
  for (int i = 0; i < 4; ++i) {
    bytes[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
  return write(bytes, sizeof(bytes));
}

Status FileWriter::write_u64(uint64_t value) {
  uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
  return write(bytes, sizeof(bytes));
}

Status FileWriter::flush() { return error_; }

Status FileWriter::sync() {
  if (!error_.ok()) return error_;
  if (fd_ < 0) return Status::success();
  if (::fsync(fd_) != 0) return errno_status("fsync " + path_);
  return Status::success();
}

Status FileWriter::close() {
  if (fd_ < 0) return error_;
  Status result = error_;
  if (::close(fd_) != 0 && result.ok()) {
    result = errno_status("close " + path_);
  }
  fd_ = -1;
  return result;
}

Status read_file(const std::string& path, size_t max_bytes,
                 std::vector<uint8_t>& out) {
  out.clear();
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno_status("open " + path);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const auto status = errno_status("stat " + path);
    ::close(fd);
    return status;
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    return Status::error(ErrorCode::InvalidArgument, path + " is not a file");
  }
  if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > max_bytes) {
    ::close(fd);
    return Status::error(ErrorCode::LimitExceeded,
                         path + " exceeds the trace size limit");
  }
  const auto size = static_cast<size_t>(st.st_size);
  out.resize(size);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t n = ::read(fd, out.data() + offset, size - offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      out.clear();
      const auto status = errno_status("read " + path);
      ::close(fd);
      return status;
    }
    if (n == 0) {
      out.resize(offset);
      ::close(fd);
      return Status::error(ErrorCode::Truncated,
                           "file shrank while reading " + path);
    }
    offset += static_cast<size_t>(n);
  }
  if (::close(fd) != 0) {
    out.clear();
    return errno_status("close " + path);
  }
  return Status::success();
}

bool is_regular_file(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

Status ensure_parent_dirs(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path p(path);
  const auto parent = p.parent_path();
  if (parent.empty()) return Status::success();
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    return Status::error(ErrorCode::IoError,
                         "cannot create directory " + parent.string() + ": " +
                             ec.message());
  }
  return Status::success();
}

}  // namespace gbx::trace
