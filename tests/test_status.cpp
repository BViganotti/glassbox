// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "glassbox/status.hpp"

TEST(status_ok) {
  gbx::Status s;
  CHECK(s.ok());
  CHECK(s.code() == gbx::ErrorCode::Ok);
  CHECK_EQ(s.describe(), std::string("ok"));
}

TEST(status_error) {
  auto s = gbx::Status::error(gbx::ErrorCode::Corrupt, "bad magic");
  CHECK(!s.ok());
  CHECK(s.code() == gbx::ErrorCode::Corrupt);
  CHECK_EQ(s.describe(), std::string("corrupt: bad magic"));
  auto wrapped = gbx::wrap(s, "opening trace");
  CHECK_EQ(wrapped.message(), std::string("opening trace: bad magic"));
}
