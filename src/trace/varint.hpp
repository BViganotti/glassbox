// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace gbx::trace {

/// Maximum bytes in an unsigned LEB128 varint for 64-bit values.
inline constexpr int kMaxVarintBytes = 10;

/// Appends values to a byte vector. All encodings are little-endian and are
/// accepted by the corresponding ByteReader.
class ByteWriter {
 public:
  explicit ByteWriter(std::vector<uint8_t>& out) : out_(out) {}

  void u8(uint8_t v) { out_.push_back(v); }

  void varint(uint64_t v) {
    while (v >= 0x80) {
      out_.push_back(static_cast<uint8_t>((v & 0x7f) | 0x80));
      v >>= 7;
    }
    out_.push_back(static_cast<uint8_t>(v));
  }

  void svarint(int64_t v) {
    // Zig-zag keeps small negative deltas compact without undefined behaviour.
    const auto u = static_cast<uint64_t>(v);
    const auto sign = static_cast<uint64_t>(v >> 63);
    varint((u << 1) ^ sign);
  }

  void f64(double v) {
    const uint64_t bits = std::bit_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
      out_.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xff));
    }
  }

  void bytes(std::span<const uint8_t> data) {
    out_.insert(out_.end(), data.begin(), data.end());
  }

  void raw(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    out_.insert(out_.end(), p, p + size);
  }

 private:
  std::vector<uint8_t>& out_;
};

/// Bounds-checked decoder. Every read fails cleanly (sticky flag) instead of
/// reading out of bounds, so malformed trace data cannot cause UB.
class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t size)
      : start_(data), p_(data), end_(data + size) {}
  explicit ByteReader(std::span<const uint8_t> data)
      : ByteReader(data.data(), data.size()) {}

  bool ok() const { return ok_; }
  size_t offset() const { return static_cast<size_t>(p_ - start_); }
  size_t remaining() const { return static_cast<size_t>(end_ - p_); }

  bool u8(uint8_t& out) {
    if (!ok_ || p_ >= end_) return fail();
    out = *p_++;
    return true;
  }

  /// Decodes at most 10 bytes, rejecting truncated and oversized encodings.
  bool varint(uint64_t& out) {
    uint64_t result = 0;
    for (int i = 0; i < kMaxVarintBytes; ++i) {
      if (!ok_ || p_ >= end_) return fail();
      const uint8_t byte = *p_++;
      if (i == 9 && (byte & 0xfe) != 0) return fail();  // > 64 bits
      result |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
      if ((byte & 0x80) == 0) {
        out = result;
        return true;
      }
    }
    return fail();
  }

  bool svarint(int64_t& out) {
    uint64_t raw = 0;
    if (!varint(raw)) return false;
    out = static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1);
    return true;
  }

  bool f64(double& out) {
    if (!ok_ || remaining() < 8) return fail();
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
      bits |= static_cast<uint64_t>(*p_++) << (i * 8);
    }
    out = std::bit_cast<double>(bits);
    return true;
  }

  /// Reads a fixed-width little-endian 32-bit value.
  bool u32(uint32_t& out) {
    if (!ok_ || remaining() < 4) return fail();
    out = static_cast<uint32_t>(p_[0]) |
          (static_cast<uint32_t>(p_[1]) << 8) |
          (static_cast<uint32_t>(p_[2]) << 16) |
          (static_cast<uint32_t>(p_[3]) << 24);
    p_ += 4;
    return true;
  }

  /// Reads a fixed-width little-endian 64-bit value.
  bool u64(uint64_t& out) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!u32(lo) || !u32(hi)) return false;
    out = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
  }

  bool raw(void* dst, size_t size) {
    if (!ok_ || remaining() < size) return fail();
    if (size > 0) std::memcpy(dst, p_, size);
    p_ += size;
    return true;
  }

  bool skip(size_t size) {
    if (!ok_ || remaining() < size) return fail();
    p_ += size;
    return true;
  }

  /// Peeks at the next `size` bytes without consuming them.
  std::span<const uint8_t> peek(size_t size) const {
    if (!ok_ || remaining() < size) return {};
    return {p_, size};
  }

  bool bytes(std::span<uint8_t> out) {
    if (!ok_ || remaining() < out.size()) return fail();
    std::memcpy(out.data(), p_, out.size());
    p_ += out.size();
    return true;
  }

 private:
  bool fail() {
    ok_ = false;
    return false;
  }

  const uint8_t* start_{nullptr};
  const uint8_t* p_{nullptr};
  const uint8_t* end_{nullptr};
  bool ok_{true};
};

}  // namespace gbx::trace
