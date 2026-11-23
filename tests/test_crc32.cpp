// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "trace/crc32.hpp"

#include <string>
#include <vector>

#include "test_framework.hpp"

TEST(crc32_known_vector) {
  const std::string data = "123456789";
  const auto* p = reinterpret_cast<const uint8_t*>(data.data());
  CHECK_EQ(gbx::trace::crc32({p, data.size()}), 0xcbf43926u);
}

TEST(crc32_empty) {
  CHECK_EQ(gbx::trace::crc32({}), 0u);
}

TEST(crc32_incremental_matches_oneshot) {
  std::vector<uint8_t> data(1000);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff);
  }
  gbx::trace::Crc32 inc;
  size_t off = 0;
  for (size_t step : {size_t{1}, size_t{7}, size_t{64}, size_t{256},
                      size_t{672}}) {
    inc.update(data.data() + off, step);
    off += step;
  }
  CHECK_EQ(off, data.size());
  CHECK_EQ(inc.value(), gbx::trace::crc32(data));
}

TEST(crc32_detects_single_bit_flip) {
  std::vector<uint8_t> data{1, 2, 3, 4, 5};
  const uint32_t before = gbx::trace::crc32(data);
  data[2] ^= 0x40;
  CHECK_NE(gbx::trace::crc32(data), before);
}
