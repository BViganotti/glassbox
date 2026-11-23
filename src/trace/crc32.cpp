// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "trace/crc32.hpp"

#include <array>

namespace gbx::trace {
namespace {

constexpr std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

constexpr auto kTable = make_table();

}  // namespace

const uint32_t* Crc32::table() { return kTable.data(); }

uint32_t crc32(std::span<const uint8_t> data) {
  Crc32 crc;
  crc.update(data);
  return crc.value();
}

}  // namespace gbx::trace
