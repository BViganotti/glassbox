// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gbx::trace {

/// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) used to detect
/// corruption in trace chunks. Implemented locally to avoid dependencies.
class Crc32 {
 public:
  void update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = state_;
    for (size_t i = 0; i < size; ++i) {
      c = table()[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    state_ = c;
  }

  void update(std::span<const uint8_t> data) { update(data.data(), data.size()); }

  uint32_t value() const { return state_ ^ 0xffffffffu; }

  void reset() { state_ = 0xffffffffu; }

 private:
  static const uint32_t* table();
  uint32_t state_{0xffffffffu};
};

uint32_t crc32(std::span<const uint8_t> data);

}  // namespace gbx::trace
