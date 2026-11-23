// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

TEST(state_history_numbers) {
  gbx::test::AnalysisFixture f;
  for (int i = 1; i <= 5; ++i) {
    f.snapshot_number(static_cast<uint64_t>(i * 10), "health",
                      100.0 - static_cast<double>(i) * 5.0);
    f.snapshot_text(static_cast<uint64_t>(i * 10), "mode", i % 2 ? "idle" : "combat");
  }
  f.finalize();
  const auto history = gbx::state_history(f.trace, "health");
  CHECK_EQ(history.size(), size_t{5});
  CHECK_EQ(history.front().ts, uint64_t{10});
  CHECK_EQ(history.back().ts, uint64_t{50});
  CHECK_NEAR(history.front().number, 95.0, 1e-9);
  CHECK_NEAR(history.back().number, 75.0, 1e-9);
  CHECK(history.front().type == gbx::ValueType::Float);
  const auto text = gbx::state_history(f.trace, "mode");
  CHECK_EQ(text.size(), size_t{5});
  CHECK(text.front().type == gbx::ValueType::String);
  CHECK_EQ(text[1].text, std::string("combat"));
}

TEST(state_history_filters_and_caps) {
  gbx::test::AnalysisFixture f;
  for (int i = 0; i < 100; ++i) {
    f.snapshot_number(static_cast<uint64_t>(i), "counter", static_cast<double>(i));
  }
  f.finalize();
  const auto capped = gbx::state_history(f.trace, "counter", 10);
  CHECK_EQ(capped.size(), size_t{10});
  CHECK_EQ(capped.front().ts, uint64_t{0});
  CHECK_EQ(capped.back().ts, uint64_t{99});
  const auto window = gbx::state_history(f.trace, "counter", 100, 20, 29);
  CHECK_EQ(window.size(), size_t{10});
  CHECK_EQ(window.front().ts, uint64_t{20});
  CHECK_EQ(window.back().ts, uint64_t{29});
  CHECK(gbx::state_history(f.trace, "missing").empty());
  CHECK(gbx::state_history(f.trace, "", 10).empty());
}

TEST(state_history_survives_duplicate_keys) {
  gbx::test::AnalysisFixture f;
  f.scratch.push_back({f.str("value"), gbx::Value::real(1.0)});
  f.scratch.push_back({f.str("value"), gbx::Value::real(2.0)});
  f.emit(5, gbx::RecordType::Snapshot);
  f.finalize();
  const auto history = gbx::state_history(f.trace, "value");
  CHECK_EQ(history.size(), size_t{2});
  CHECK_NEAR(history[0].number, 1.0, 1e-9);
  CHECK_NEAR(history[1].number, 2.0, 1e-9);
}
