// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/trace.hpp"

#include <string>

#include "test_framework.hpp"

TEST(string_table_interning) {
  gbx::StringTable table;
  const auto a = table.try_intern("alpha");
  const auto b = table.try_intern("beta");
  const auto again = table.try_intern("alpha");
  CHECK(a.has_value());
  CHECK(b.has_value());
  CHECK(again.has_value());
  CHECK_EQ(*a, *again);
  CHECK_NE(*a, *b);
  CHECK_EQ(table.get(*a), std::string_view("alpha"));
  CHECK_EQ(table.get(*b), std::string_view("beta"));
  CHECK_EQ(table.get(0), std::string_view(""));
  CHECK_EQ(table.size(), size_t{3});  // "" + alpha + beta
  CHECK_EQ(table.find("beta").value_or(999), *b);
  CHECK(!table.find("gamma").has_value());
}

TEST(string_table_limits) {
  gbx::StringTableLimits limits;
  limits.max_strings = 3;
  limits.max_bytes = 8;
  limits.max_string_length = 4;
  gbx::StringTable table(limits);
  CHECK(table.try_intern("abcd").has_value());
  CHECK(!table.try_intern("abcde").has_value());  // too long
  CHECK(table.try_intern("e").has_value());
  CHECK(!table.try_intern("f").has_value());  // max_strings hit
  CHECK(table.find("f") == std::nullopt);
  CHECK(table.full());
}

TEST(string_table_byte_limit) {
  gbx::StringTableLimits limits;
  limits.max_bytes = 5;
  gbx::StringTable table(limits);
  CHECK(table.try_intern("abcde").has_value());
  CHECK(!table.try_intern("fg").has_value());
  CHECK_EQ(table.size(), size_t{2});
}

TEST(string_table_out_of_range) {
  gbx::StringTable table;
  CHECK_EQ(table.get(999), std::string_view(""));
  CHECK_EQ(table.get(gbx::StringTable::kInvalidId), std::string_view(""));
}

TEST(string_table_survives_growth) {
  gbx::StringTable table;
  for (int i = 0; i < 500; ++i) {
    CHECK(table.try_intern("value-" + std::to_string(i)).has_value());
  }
  CHECK_EQ(table.get(1), std::string_view("value-0"));
  CHECK_EQ(table.get(500), std::string_view("value-499"));
  CHECK_EQ(table.find("value-123").value_or(0), uint32_t{124});
}
