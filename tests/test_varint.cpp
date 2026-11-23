// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "trace/varint.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include "test_framework.hpp"

namespace {

using gbx::trace::ByteReader;
using gbx::trace::ByteWriter;

uint64_t roundtrip(uint64_t v) {
  std::vector<uint8_t> buf;
  ByteWriter writer(buf);
  writer.varint(v);
  ByteReader reader(buf);
  uint64_t out = 0;
  CHECK(reader.varint(out));
  CHECK_EQ(reader.remaining(), size_t{0});
  return out;
}

}  // namespace

TEST(varint_boundaries) {
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{127}, uint64_t{128},
                     uint64_t{300}, uint64_t{16384},
                     uint64_t{0xffffffffu},
                     std::numeric_limits<uint64_t>::max()}) {
    CHECK_EQ(roundtrip(v), v);
  }
}

TEST(varint_known_encoding) {
  std::vector<uint8_t> buf;
  ByteWriter writer(buf);
  writer.varint(300);
  CHECK_EQ(buf.size(), size_t{2});
  CHECK_EQ(buf[0], uint8_t{0xac});
  CHECK_EQ(buf[1], uint8_t{0x02});
}

TEST(varint_zigzag) {
  for (int64_t v : {int64_t{0}, int64_t{-1}, int64_t{1}, int64_t{-64},
                    int64_t{123456789}, std::numeric_limits<int64_t>::min(),
                    std::numeric_limits<int64_t>::max()}) {
    std::vector<uint8_t> buf;
    ByteWriter writer(buf);
    writer.svarint(v);
    ByteReader reader(buf);
    int64_t out = 0;
    CHECK(reader.svarint(out));
    CHECK_EQ(out, v);
  }
}

TEST(varint_rejects_truncation) {
  std::vector<uint8_t> buf{0x80, 0x80};
  ByteReader reader(buf);
  uint64_t out = 0;
  CHECK(!reader.varint(out));
  CHECK(!reader.ok());
  CHECK(!reader.varint(out));  // sticky failure
}

TEST(varint_rejects_overflow) {
  std::vector<uint8_t> buf(10, 0xff);
  ByteReader reader(buf);
  uint64_t out = 0;
  CHECK(!reader.varint(out));
}

TEST(varint_rejects_11_bytes) {
  std::vector<uint8_t> buf(11, 0x80);
  buf.push_back(0x01);
  ByteReader reader(buf);
  uint64_t out = 0;
  CHECK(!reader.varint(out));
}

TEST(double_roundtrip) {
  for (double v : {0.0, -0.0, 1.5, -3.25, 1e300, -1e-300}) {
    std::vector<uint8_t> buf;
    ByteWriter writer(buf);
    writer.f64(v);
    ByteReader reader(buf);
    double out = 0;
    CHECK(reader.f64(out));
    CHECK_EQ(std::bit_cast<uint64_t>(out), std::bit_cast<uint64_t>(v));
  }
}

TEST(reader_never_reads_past_end) {
  std::vector<uint8_t> buf{0x01};
  ByteReader reader(buf);
  double d = 0;
  CHECK(!reader.f64(d));
  CHECK(!reader.ok());
  CHECK_EQ(reader.remaining(), size_t{1});
}
