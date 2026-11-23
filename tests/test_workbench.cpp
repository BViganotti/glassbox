// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/workbench.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "glassbox/demo.hpp"
#include "glassbox/json.hpp"
#include "glassbox/trace_io.hpp"
#include "http_client.hpp"
#include "test_framework.hpp"

namespace {

std::string temp_dir(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-workbench-" + std::to_string(::getpid()) + "-" +
                    name);
  std::filesystem::create_directories(dir);
  return dir.string();
}

gbx::WorkbenchOptions options_for(const std::string& dir) {
  gbx::WorkbenchOptions options;
  options.trace_dir = dir;
  options.extra_trace_dirs = {dir};
  options.auto_record_demo = false;
  options.open_browser = false;
  options.port = 0;
  options.app_name = "workbench-test";
  return options;
}

gbx::json::Value parse(const std::string& text) {
  gbx::json::Value value;
  const auto status = gbx::json::parse(text, value);
  CHECK_MSG(status.ok(), status.describe());
  return value;
}

}  // namespace

TEST(workbench_serves_ui_and_enforces_auth) {
  const std::string dir = temp_dir("auth");
  gbx::Workbench workbench(options_for(dir));
  auto status = workbench.start();
  CHECK_MSG(status.ok(), status.describe());
  CHECK(workbench.running());
  CHECK_GT(workbench.port(), uint16_t{0});
  CHECK_EQ(workbench.token().size(), size_t{32});
  CHECK(workbench.url().find("?t=" + workbench.token()) != std::string::npos);

  auto response = gbx::test::http_request(workbench.port(), "GET", "/api/health");
  CHECK_EQ(response.status, 403);

  response = gbx::test::http_request(workbench.port(), "GET", "/api/health", {},
                                     workbench.token());
  CHECK_EQ(response.status, 200);
  CHECK(gbx::test::json_ok(response.body, "ok"));

  response = gbx::test::http_request(workbench.port(), "GET", "/api/health",
                                     {}, workbench.token(), {}, "evil.example");
  CHECK_EQ(response.status, 403);

  response = gbx::test::http_request(
      workbench.port(), "POST", "/api/shutdown", "{}", workbench.token(),
      "Origin: https://evil.example\r\n");
  CHECK_EQ(response.status, 403);

  response = gbx::test::http_request(workbench.port(), "GET", "/");
  CHECK_EQ(response.status, 403);

  response = gbx::test::http_request(
      workbench.port(), "GET", "/?t=" + workbench.token());
  CHECK_EQ(response.status, 200);
  CHECK(response.body.find("<title>Glassbox</title>") != std::string::npos);
  CHECK(response.header("content-security-policy").find("default-src") !=
        std::string::npos);
  const std::string cookie = response.header("set-cookie");
  CHECK(cookie.find("gb_token=") != std::string::npos);

  response = gbx::test::http_request(workbench.port(), "GET", "/app.js", {}, "",
                                     "Cookie: " + cookie.substr(0, cookie.find(';')) +
                                         "\r\n");
  CHECK_EQ(response.status, 200);
  CHECK(response.body.find("Glassbox") != std::string::npos ||
        response.body.find("glassbox") != std::string::npos);

  response = gbx::test::http_request(workbench.port(), "GET", "/../etc/passwd",
                                     {}, workbench.token());
  CHECK_EQ(response.status, 400);
  workbench.stop();
  CHECK(!workbench.running());
}

TEST(workbench_opens_trace_and_serves_analysis) {
  const std::string dir = temp_dir("analysis");
  const std::string trace_path = dir + "/fixture.gbt";
  gbx::DemoOptions demo;
  demo.frames = 40;
  demo.sleep_spike = false;
  const auto recorded = gbx::record_demo_trace(trace_path, demo);
  CHECK(recorded.status.ok());

  gbx::Workbench workbench(options_for(dir));
  CHECK(workbench.start().ok());
  const auto token = workbench.token();
  const auto port = workbench.port();

  auto response = gbx::test::http_request(
      port, "POST", "/api/open",
      "{\"path\":\"" + trace_path + "\"}", token);
  CHECK_MSG(response.status == 200, response.body);
  CHECK(gbx::test::json_ok(response.body, "ok"));

  response = gbx::test::http_request(port, "GET", "/api/traces", {}, token);
  CHECK_EQ(response.status, 200);
  CHECK(response.body.find("fixture.gbt") != std::string::npos);

  response = gbx::test::http_request(port, "GET", "/api/meta", {}, token);
  CHECK_EQ(response.status, 200);
  auto meta = parse(response.body);
  CHECK(meta.find("records") != nullptr);
  CHECK_GT(meta.find("records")->as_number(), 100.0);
  CHECK_EQ(meta.find("name")->as_string(), std::string("fixture.gbt"));

  response = gbx::test::http_request(port, "GET",
                                     "/api/timeline?buckets=32", {}, token);
  CHECK_EQ(response.status, 200);
  auto timeline = parse(response.body);
  CHECK_EQ(timeline.find("buckets")->array.size(), size_t{32});

  response = gbx::test::http_request(
      port, "GET", "/api/events?limit=5&types=log", {}, token);
  CHECK_EQ(response.status, 200);
  auto events = parse(response.body);
  CHECK_EQ(events.find("records")->array.size(), size_t{5});
  CHECK_EQ(events.find("records")->array[0].find("type")->as_string(),
           std::string("log"));

  response = gbx::test::http_request(
      port, "GET", "/api/events?q=type%3Dalloc%20AND%20size%3E100", {}, token);
  CHECK_EQ(response.status, 200);
  response = gbx::test::http_request(port, "GET", "/api/events?q=type%3D", {},
                                     token);
  CHECK_EQ(response.status, 400);

  response = gbx::test::http_request(port, "GET", "/api/heap?limit=10", {},
                                     token);
  CHECK_EQ(response.status, 200);
  auto heap = parse(response.body);
  CHECK_GT(heap.find("total_bytes")->as_number(), 0.0);
  CHECK_LE(heap.find("objects")->array.size(), size_t{10});

  response = gbx::test::http_request(port, "GET", "/api/heap/graph?nodes=500",
                                     {}, token);
  CHECK_EQ(response.status, 200);
  auto graph = parse(response.body);
  CHECK(graph.find("converged")->as_boolean());

  response = gbx::test::http_request(port, "GET", "/api/leaks", {}, token);
  CHECK_EQ(response.status, 200);
  CHECK(parse(response.body).find("leaks")->array.size() > 0);

  response = gbx::test::http_request(port, "GET", "/api/flame?nodes=2000", {},
                                     token);
  CHECK_EQ(response.status, 200);
  auto flame = parse(response.body);
  CHECK(!flame.find("root")->is_null());

  response = gbx::test::http_request(port, "GET", "/api/state", {}, token);
  CHECK_EQ(response.status, 200);
  CHECK(parse(response.body).find("entries")->array.size() > 0);

  response = gbx::test::http_request(port, "GET", "/api/counters?points=32", {},
                                     token);
  CHECK_EQ(response.status, 200);
  CHECK(parse(response.body).find("series")->array.size() > 0);

  response = gbx::test::http_request(
      port, "GET", "/api/diff?a=0&b=1000000000", {}, token);
  CHECK_EQ(response.status, 200);
  CHECK(parse(response.body).find("heap")->is_array());

  response = gbx::test::http_request(
      port, "GET", "/api/query?q=inventory&limit=4", {}, token);
  CHECK_EQ(response.status, 200);
  auto query = parse(response.body);
  CHECK_LE(query.find("records")->array.size(), size_t{4});

  response = gbx::test::http_request(port, "GET", "/api/logs?level=2", {},
                                     token);
  CHECK_EQ(response.status, 200);

  response = gbx::test::http_request(port, "GET", "/api/spans?at=1", {}, token);
  CHECK_EQ(response.status, 200);
  CHECK(parse(response.body).find("spans")->is_array());

  response = gbx::test::http_request(port, "GET", "/api/spans", {}, token);
  CHECK_EQ(response.status, 200);
  auto spans = parse(response.body);
  CHECK(spans.find("returned")->as_number() > 0);
  const auto& span_list = spans.find("spans")->array;
  CHECK(!span_list.empty());
  const auto span_id = span_list[0].find("id")->as_number();
  response = gbx::test::http_request(
      port, "GET", "/api/span?id=" + std::to_string(static_cast<int>(span_id)),
      {}, token);
  CHECK_EQ(response.status, 200);

  response = gbx::test::http_request(port, "GET", "/api/event?id=0", {}, token);
  CHECK_EQ(response.status, 200);
  response = gbx::test::http_request(port, "GET", "/api/event?id=999999", {},
                                     token);
  CHECK_EQ(response.status, 404);

  response = gbx::test::http_request(
      port, "GET", "/api/state/history?key=inventory_count", {}, token);
  CHECK_EQ(response.status, 200);
  auto history = parse(response.body);
  CHECK(history.find("points")->array.size() > 0);
  CHECK_EQ(history.find("points")->array[0].find("type")->as_string(),
           std::string("number"));
  response = gbx::test::http_request(port, "GET", "/api/state/history", {},
                                     token);
  CHECK_EQ(response.status, 400);

  response = gbx::test::http_request(port, "GET", "/api/export", {}, token);
  CHECK_EQ(response.status, 200);
  auto exported = parse(response.body);
  CHECK_EQ(exported.find("format")->as_string(),
           std::string("glassbox-moment-1"));
  CHECK(exported.find("heap")->find("total_bytes")->as_number() > 0);
  CHECK(exported.find("active_spans")->is_array());
  CHECK(exported.find("state")->is_array());
  CHECK(response.header("content-disposition").find("attachment") !=
        std::string::npos);

  response = gbx::test::http_request(port, "HEAD", "/api/health", {}, token);
  CHECK_EQ(response.status, 200);
  CHECK(response.body.empty());

  workbench.stop();
}

TEST(workbench_rejects_traces_outside_roots) {
  const std::string dir = temp_dir("roots");
  const std::string outside_dir = temp_dir("outside");
  const std::string outside = outside_dir + "/outside.gbt";
  gbx::DemoOptions demo;
  demo.frames = 10;
  demo.sleep_spike = false;
  CHECK(gbx::record_demo_trace(outside, demo).status.ok());

  gbx::Workbench workbench(options_for(dir));
  CHECK(workbench.start().ok());
  auto response = gbx::test::http_request(
      workbench.port(), "POST", "/api/open",
      "{\"path\":\"" + outside + "\"}", workbench.token());
  CHECK_EQ(response.status, 400);
  CHECK(response.body.find("outside the allowed") != std::string::npos);

  response = gbx::test::http_request(
      workbench.port(), "POST", "/api/open", "{\"path\":\"/etc/passwd\"}",
      workbench.token());
  CHECK_EQ(response.status, 400);

  response = gbx::test::http_request(
      workbench.port(), "POST", "/api/open", "not json", workbench.token());
  CHECK_EQ(response.status, 400);

  response = gbx::test::http_request(
      workbench.port(), "POST", "/api/open", "{}", workbench.token());
  CHECK_EQ(response.status, 400);
  workbench.stop();
}

TEST(workbench_live_demo_streams_and_reopens) {
  const std::string dir = temp_dir("live");
  gbx::Workbench workbench(options_for(dir));
  CHECK(workbench.start().ok());
  const auto token = workbench.token();

  auto response = gbx::test::http_request(
      workbench.port(), "POST", "/api/demo",
      "{\"frames\":18,\"sampler\":false,\"sleep_spike\":false}", token);
  CHECK_MSG(response.status == 200, response.body);
  CHECK(gbx::test::json_ok(response.body, "started"));

  const std::string stream = gbx::test::sse_read_until(
      workbench.port(), "/api/live?t=" + token, "event: done", 4 * 1024 * 1024,
      8000);
  CHECK(stream.find("Content-Type: text/event-stream") != std::string::npos);
  CHECK(stream.find("event: record") != std::string::npos);
  CHECK_MSG(stream.find("event: done") != std::string::npos,
            "stream bytes=" + std::to_string(stream.size()) + " tail=" +
                stream.substr(stream.size() > 300 ? stream.size() - 300 : 0));
  CHECK_MSG(stream.find("\"frames\":18") != std::string::npos,
            "stream bytes=" + std::to_string(stream.size()));

  response = gbx::test::http_request(workbench.port(), "GET", "/api/meta", {},
                                     token);
  CHECK_EQ(response.status, 200);
  auto meta = parse(response.body);
  CHECK_EQ(meta.find("name")->as_string(), std::string("demo-live.gbt"));

  // Shutdown through the UI and confirm the loop would exit.
  response = gbx::test::http_request(workbench.port(), "POST", "/api/shutdown",
                                     "{}", token);
  CHECK_EQ(response.status, 200);
  CHECK(!workbench.running());
  workbench.stop();
}

TEST(workbench_start_twice_is_rejected) {
  const std::string dir = temp_dir("twice");
  gbx::Workbench workbench(options_for(dir));
  CHECK(workbench.start().ok());
  auto status = workbench.start();
  CHECK(!status.ok());
  CHECK(status.code() == gbx::ErrorCode::Busy);
  workbench.stop();
}
