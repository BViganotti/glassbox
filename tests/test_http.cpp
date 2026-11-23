// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "ui/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "glassbox/json.hpp"
#include "test_framework.hpp"

namespace {

int connect_to(uint16_t port) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) ==
        0) {
      struct timeval tv {};
      tv.tv_sec = 3;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      return fd;
    }
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return -1;
}

std::string exchange(uint16_t port, const std::string& request,
                     bool read_all = true) {
  const int fd = connect_to(port);
  if (fd < 0) return "<connect failed>";
  gbx::ui::HttpServer::write_all(fd, request);
  std::string response;
  char buffer[4096];
  while (true) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    response.append(buffer, static_cast<size_t>(n));
    if (!read_all && response.find("\r\n\r\n") != std::string::npos) break;
  }
  ::close(fd);
  return response;
}

std::string make_request(const std::string& method, const std::string& path,
                         const std::string& body = {},
                         const std::string& extra_headers = {}) {
  std::string request = method + " " + path + " HTTP/1.1\r\n";
  request += "Host: 127.0.0.1\r\n";
  if (!body.empty() || method == "POST") {
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Content-Type: application/json\r\n";
  }
  request += extra_headers;
  request += "Connection: close\r\n\r\n";
  request += body;
  return request;
}

std::string body_of(const std::string& response) {
  const size_t marker = response.find("\r\n\r\n");
  return marker == std::string::npos ? std::string()
                                     : response.substr(marker + 4);
}

bool has_status(const std::string& response, int status) {
  return response.rfind("HTTP/1.1 " + std::to_string(status) + " ", 0) == 0;
}

std::string json_string(const std::string& json_text, const char* key) {
  gbx::json::Value value;
  if (!gbx::json::parse(json_text, value).ok()) return {};
  const auto* field = value.find(key);
  return field != nullptr ? field->as_string() : std::string();
}

}  // namespace

TEST(http_server_routes_and_json) {
  gbx::ui::HttpServer server;
  CHECK(server.listen("127.0.0.1", 0).ok());
  CHECK_GT(server.port(), uint16_t{0});
  server.route("GET", "/hello", [](const gbx::ui::HttpRequest& request,
                                   gbx::ui::HttpResponse& response) {
    gbx::json::Writer writer;
    writer.begin_object();
    writer.key_string("message", "hi");
    writer.key_string("client", request.client);
    writer.key("query");
    writer.begin_array();
    for (const auto& [key, value] : request.query) {
      writer.begin_object();
      writer.key_string("key", key);
      writer.key_string("value", value);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    response.set_body(writer.str());
  });
  server.route("POST", "/echo", [](const gbx::ui::HttpRequest& request,
                                   gbx::ui::HttpResponse& response) {
    response.set_body("{\"len\":" + std::to_string(request.body.size()) + "}");
  });
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  auto response = exchange(
      server.port(), make_request("GET", "/hello?a=1&b=two%20words"));
  CHECK_MSG(has_status(response, 200), response.substr(0, 80));
  CHECK(response.find("Content-Type: application/json") != std::string::npos);
  CHECK_EQ(json_string(body_of(response), "message"), std::string("hi"));
  CHECK_EQ(json_string(body_of(response), "client"), std::string("127.0.0.1"));
  CHECK(body_of(response).find("\"value\":\"two words\"") != std::string::npos);

  response = exchange(server.port(), make_request("POST", "/echo", "abcdef"));
  CHECK(has_status(response, 200));
  CHECK_EQ(body_of(response), std::string("{\"len\":6}"));

  response = exchange(server.port(), make_request("GET", "/missing"));
  CHECK_MSG(has_status(response, 404), response.substr(0, 120));

  response = exchange(server.port(), make_request("DELETE", "/hello"));
  CHECK(has_status(response, 405));

  response = exchange(server.port(), "BOGUS\r\n\r\n");
  CHECK(has_status(response, 400));

  response = exchange(server.port(), make_request("GET", "/%zz"));
  CHECK(has_status(response, 400));

  server.request_stop();
  thread.join();
}

TEST(http_server_enforces_limits) {
  gbx::ui::HttpLimits limits;
  limits.max_body_bytes = 64;
  limits.max_header_bytes = 4096;
  limits.max_connections = 8;
  gbx::ui::HttpServer server(limits);
  CHECK(server.listen("127.0.0.1", 0).ok());
  server.route("POST", "/echo", [](const gbx::ui::HttpRequest& request,
                                   gbx::ui::HttpResponse& response) {
    response.set_body("{\"len\":" + std::to_string(request.body.size()) + "}");
  });
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  auto response =
      exchange(server.port(), make_request("POST", "/echo", std::string(100, 'x')));
  CHECK(has_status(response, 413));

  std::string many_headers = "Host: 127.0.0.1\r\n";
  for (int i = 0; i < 120; ++i) {
    many_headers += "X-Pad-" + std::to_string(i) + ": y\r\n";
  }
  response = exchange(server.port(),
                      "GET /hello HTTP/1.1\r\n" + many_headers + "\r\n");
  CHECK(has_status(response, 431));

  response = exchange(
      server.port(),
      "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
      "Content-Length: 2\r\n\r\nx");
  CHECK(has_status(response, 400));

  response = exchange(server.port(),
                      "POST /echo HTTP/1.1\r\nHost: x\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
  CHECK_MSG(has_status(response, 501), response.substr(0, 120));

  server.request_stop();
  thread.join();
}

TEST(http_server_sse_rejection_and_empty_stream) {
  gbx::ui::HttpServer server;
  CHECK(server.listen("127.0.0.1", 0).ok());
  server.stream_route(
      "/denied", [](const gbx::ui::HttpRequest&,
                    gbx::ui::ConnectionStream& stream) {
        stream.reject(403, "no token");
      });
  server.stream_route(
      "/empty", [](const gbx::ui::HttpRequest&,
                   gbx::ui::ConnectionStream&) {});
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto response = exchange(server.port(), make_request("GET", "/denied"));
  CHECK(has_status(response, 403));
  CHECK(body_of(response).find("no token") != std::string::npos);
  response = exchange(server.port(), make_request("GET", "/empty"));
  CHECK(has_status(response, 204));
  server.request_stop();
  thread.join();
}

TEST(http_server_sse_stream) {
  gbx::ui::HttpServer server;
  CHECK(server.listen("127.0.0.1", 0).ok());
  server.stream_route(
      "/stream", [](const gbx::ui::HttpRequest&,
                    gbx::ui::ConnectionStream& stream) {
        stream.comment("hello");
        stream.sse_event("tick", "{\"n\":1}");
        stream.sse_event("tick", "{\"n\":2}");
      });
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  const int fd = connect_to(server.port());
  CHECK_GE(fd, 0);
  if (fd >= 0) {
    gbx::ui::HttpServer::write_all(fd,
                                   make_request("GET", "/stream"));
    std::string response;
    char buffer[2048];
    while (true) {
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0) break;
      response.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);
    CHECK(response.find("Content-Type: text/event-stream") != std::string::npos);
    CHECK(response.find(": hello") != std::string::npos);
    size_t ticks = 0;
    size_t pos = 0;
    while ((pos = response.find("event: tick", pos)) != std::string::npos) {
      ++ticks;
      pos += 11;
    }
    CHECK_EQ(ticks, size_t{2});
    CHECK(response.find("data: {\"n\":2}") != std::string::npos);
  }

  server.request_stop();
  thread.join();
}

TEST(http_server_bad_requests_do_not_crash) {
  gbx::ui::HttpServer server;
  CHECK(server.listen("localhost", 0).ok());
  server.route("GET", "/ok", [](const gbx::ui::HttpRequest&,
                                gbx::ui::HttpResponse& response) {
    response.set_body("{}");
  });
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const char* malformed[] = {
      "\r\n\r\n",
      "GET\r\n\r\n",
      "GET /ok\r\n\r\n",
      "GET /ok HTTP/9.9\r\nHost: x\r\n\r\n",
      "GET /ok HTTP/1.1\r\nBad Header\r\n\r\n",
  };
  for (const char* request : malformed) {
    const auto response = exchange(server.port(), request);
    CHECK(!response.empty());
    CHECK(response.rfind("HTTP/1.1 ", 0) == 0);
  }

  // A truncated body is detected when the client closes its write side.
  const int fd = connect_to(server.port());
  CHECK_GE(fd, 0);
  if (fd >= 0) {
    gbx::ui::HttpServer::write_all(
        fd, "POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab");
    ::shutdown(fd, SHUT_WR);
    std::string response;
    char buffer[1024];
    while (true) {
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0) break;
      response.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);
    CHECK(response.rfind("HTTP/1.1 400 ", 0) == 0);
  }
  server.request_stop();
  thread.join();
}

TEST(http_server_rejects_excess_params_and_long_targets) {
  gbx::ui::HttpServer server;
  CHECK(server.listen("127.0.0.1", 0).ok());
  server.route("GET", "/ok", [](const gbx::ui::HttpRequest&,
                                gbx::ui::HttpResponse& response) {
    response.set_body("{}");
  });
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  std::string path = "/ok?";
  for (int i = 0; i < 40; ++i) path += "k" + std::to_string(i) + "=v&";
  auto response = exchange(server.port(), make_request("GET", path));
  CHECK_MSG(has_status(response, 400), response.substr(0, 80));

  const std::string long_path = "/" + std::string(5000, 'a');
  response = exchange(server.port(), make_request("GET", long_path));
  CHECK_MSG(has_status(response, 400), response.substr(0, 80));

  server.request_stop();
  thread.join();
}

TEST(http_server_connection_cap_returns_503) {
  gbx::ui::HttpLimits limits;
  limits.max_connections = 1;
  limits.io_timeout_ms = 700;
  gbx::ui::HttpServer server(limits);
  CHECK(server.listen("127.0.0.1", 0).ok());
  server.route("GET", "/ok", [](const gbx::ui::HttpRequest&,
                                gbx::ui::HttpResponse& response) {
    response.set_body("{}");
  });
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  const int held = connect_to(server.port());
  CHECK_GE(held, 0);
  if (held >= 0) {
    gbx::ui::HttpServer::write_all(held,
                                   "GET /ok HTTP/1.1\r\nHost: 127.0.0.1\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto response = exchange(server.port(), make_request("GET", "/ok"));
    CHECK_MSG(has_status(response, 503), response.substr(0, 80));
    ::close(held);
  }
  server.request_stop();
  thread.join();
}
