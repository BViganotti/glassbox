// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/analysis.hpp"

#include <string>

#include "analysis_fixture.hpp"
#include "test_framework.hpp"

TEST(counter_series_aggregation) {
  gbx::test::AnalysisFixture f;
  f.counter(0, "fps", 60.0, "hz");
  f.counter(10, "fps", 58.0, "hz");
  f.counter(20, "fps", 61.5, "hz");
  f.gauge(5, "heap_bytes", 1024.0);
  f.counter(15, "draw_calls", 30.0);
  f.finalize();
  const auto series = gbx::counter_series(f.trace);
  CHECK_EQ(series.size(), size_t{3});
  CHECK_EQ(series[0].name, std::string("draw_calls"));
  CHECK_EQ(series[1].name, std::string("fps"));
  CHECK_EQ(series[1].unit, std::string("hz"));
  CHECK_EQ(series[1].points.size(), size_t{3});
  CHECK_NEAR(series[1].minimum, 58.0, 1e-9);
  CHECK_NEAR(series[1].maximum, 61.5, 1e-9);
  CHECK_NEAR(series[1].last, 61.5, 1e-9);
  CHECK(series[2].kind == gbx::RecordType::Gauge);
}

TEST(counter_series_filter_and_downsample) {
  gbx::test::AnalysisFixture f;
  for (int i = 0; i < 100; ++i) {
    f.counter(static_cast<uint64_t>(i), "fps", static_cast<double>(i));
    f.counter(static_cast<uint64_t>(i), "heap", static_cast<double>(i * 2));
  }
  f.finalize();
  const auto fps = gbx::counter_series(f.trace, "FP", 8, 10);
  CHECK_EQ(fps.size(), size_t{1});
  CHECK_EQ(fps[0].points.size(), size_t{10});
  CHECK_NEAR(fps[0].points.front().value, 0.0, 1e-9);
  CHECK_NEAR(fps[0].points.back().value, 99.0, 1e-9);
  const auto none = gbx::counter_series(f.trace, "nothing", 8, 10);
  CHECK(none.empty());
}

TEST(counter_value_at_time) {
  gbx::test::AnalysisFixture f;
  f.counter(10, "fps", 60.0);
  f.counter(20, "fps", 50.0);
  f.gauge(15, "heap", 4096.0);
  f.finalize();
  double value = 0;
  CHECK(!gbx::series_value_at(f.trace, "fps", 9, value));
  CHECK(gbx::series_value_at(f.trace, "fps", 10, value));
  CHECK_NEAR(value, 60.0, 1e-9);
  CHECK(gbx::series_value_at(f.trace, "fps", 15, value));
  CHECK_NEAR(value, 60.0, 1e-9);
  CHECK(gbx::series_value_at(f.trace, "fps", 100, value));
  CHECK_NEAR(value, 50.0, 1e-9);
  CHECK(gbx::series_value_at(f.trace, "heap", 20, value));
  CHECK_NEAR(value, 4096.0, 1e-9);
  CHECK(!gbx::series_value_at(f.trace, "missing", 100, value));
}

TEST(state_at_merges_snapshots) {
  gbx::test::AnalysisFixture f;
  f.snapshot_number(10, "health", 100);
  f.snapshot_text(10, "mode", "idle");
  f.snapshot_number(20, "health", 90);
  f.snapshot_text(20, "mode", "combat");
  f.snapshot_text(30, "mode", "dead");
  f.snapshot_text(30, "last_error", "core dump");
  f.finalize();

  auto at5 = gbx::state_at(f.trace, 5);
  CHECK(at5.empty());

  auto at15 = gbx::state_at(f.trace, 15);
  CHECK_EQ(at15.size(), size_t{2});
  CHECK_EQ(at15[0].key, std::string("health"));
  CHECK_NEAR(at15[0].number, 100.0, 1e-9);
  CHECK_EQ(at15[1].key, std::string("mode"));
  CHECK_EQ(gbx::format_state_entry(at15[1]), std::string("idle"));

  auto at25 = gbx::state_at(f.trace, 25);
  CHECK_EQ(at25.size(), size_t{2});
  CHECK_NEAR(at25[0].number, 90.0, 1e-9);
  CHECK_EQ(gbx::format_state_entry(at25[1]), std::string("combat"));

  auto at100 = gbx::state_at(f.trace, 100);
  CHECK_EQ(at100.size(), size_t{3});
  CHECK_EQ(at100[0].key, std::string("health"));
  CHECK_EQ(at100[1].key, std::string("last_error"));
  CHECK_EQ(at100[1].text, std::string("core dump"));
  CHECK_EQ(at100[2].key, std::string("mode"));
  CHECK_EQ(gbx::format_state_entry(at100[2]), std::string("dead"));
}

TEST(state_last_write_wins_within_snapshot) {
  gbx::test::AnalysisFixture f;
  f.scratch.push_back({f.str("value"), gbx::Value::real(1.0)});
  f.scratch.push_back({f.str("value"), gbx::Value::real(2.0)});
  f.emit(5, gbx::RecordType::Snapshot);
  f.finalize();
  const auto state = gbx::state_at(f.trace, 5);
  CHECK_EQ(state.size(), size_t{1});
  CHECK_NEAR(state[0].number, 2.0, 1e-9);
}
