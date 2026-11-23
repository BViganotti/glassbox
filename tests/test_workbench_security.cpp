// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/workbench.hpp"

#include <unistd.h>

#include <filesystem>
#include <string>

#include "glassbox/demo.hpp"
#include "http_client.hpp"
#include "test_framework.hpp"

namespace {

std::string temp_dir(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-sec-" + std::to_string(::getpid()) + "-" + name);
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
  return options;
}

}  // namespace

TEST(workbench_accepts_cookie_and_rejects_bad_origins) {
  const std::string dir = temp_dir("cookie");
  gbx::Workbench workbench(options_for(dir));
  CHECK(workbench.start().ok());
  const uint16_t port = workbench.port();
  const std::string token = workbench.token();

  auto response = gbx::test::http_request(
      port, "GET", "/", {}, "", {}, "127.0.0.1:" + std::to_string(port));
  CHECK_EQ(response.status, 403);
  response = gbx::test::http_request(
      port, "GET", "/?t=" + token, {}, "",
      "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n");
  CHECK_EQ(response.status, 200);
  const std::string set_cookie = response.header("set-cookie");
  CHECK(set_cookie.find("gb_token=") != std::string::npos);
  const std::string cookie = set_cookie.substr(0, set_cookie.find(';'));
  response = gbx::test::http_request(port, "GET", "/api/health", {}, "",
                                     "Cookie: " + cookie + "\r\n");
  CHECK_EQ(response.status, 200);
  CHECK(gbx::test::json_ok(response.body, "ok"));

  response = gbx::test::http_request(
      port, "GET", "/api/health", {}, token,
      "Origin: http://localhost:9999\r\n");
  CHECK_EQ(response.status, 403);
  response = gbx::test::http_request(port, "GET", "/api/nope", {}, token);
  CHECK_EQ(response.status, 404);
  response = gbx::test::http_request(port, "GET", "/api/live");
  CHECK_EQ(response.status, 403);
  CHECK(response.body.find("session token") != std::string::npos);
  response = gbx::test::http_request(port, "POST", "/api/health", "{}", token);
  CHECK_EQ(response.status, 404);
  workbench.stop();
}

TEST(workbench_rejects_symlinks_escaping_roots) {
  const std::string dir = temp_dir("symlink-root");
  const std::string outside_dir = temp_dir("symlink-outside");
  const std::string outside_trace = outside_dir + "/outside.gbt";
  gbx::DemoOptions demo;
  demo.frames = 8;
  demo.sleep_spike = false;
  CHECK(gbx::record_demo_trace(outside_trace, demo).status.ok());

  const std::string link = dir + "/linked.gbt";
  std::error_code ec;
  std::filesystem::remove(link, ec);
  std::filesystem::create_symlink(outside_trace, link, ec);
  CHECK_MSG(!ec, ec.message());

  gbx::Workbench workbench(options_for(dir));
  CHECK(workbench.start().ok());
  auto response = gbx::test::http_request(
      workbench.port(), "POST", "/api/open",
      "{\"path\":\"" + link + "\"}", workbench.token());
  CHECK_EQ(response.status, 400);
  CHECK(response.body.find("outside the allowed") != std::string::npos);

  // The listing resolves the link and must not expose it either.
  response = gbx::test::http_request(workbench.port(), "GET", "/api/traces", {},
                                     workbench.token());
  CHECK_EQ(response.status, 200);
  CHECK(response.body.find("linked.gbt") == std::string::npos);
  workbench.stop();
}
