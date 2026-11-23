// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "glassbox/status.hpp"

namespace gbx::ui {

struct HttpLimits {
  size_t max_header_bytes = 32 * 1024;
  size_t max_body_bytes = 1 * 1024 * 1024;
  int max_connections = 64;
  int io_timeout_ms = 10000;
};

struct HttpRequest {
  std::string method;
  std::string target;
  std::string path;
  std::string query_string;
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercase
  std::vector<std::pair<std::string, std::string>> query;
  std::string body;
  std::string client;

  std::string header(std::string_view name) const;
  std::string query_value(std::string_view name) const;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  void set_body(std::string text, std::string_view type = {});
  void set_error(int code, std::string message);
};

/// SSE connection handed to a stream handler after response headers are sent.
class ConnectionStream {
 public:
  bool write(std::string_view data);
  bool sse_event(std::string_view event, std::string_view data);
  bool comment(std::string_view text);
  bool alive() const;
  uint64_t bytes_sent() const { return bytes_; }

 private:
  friend class HttpServer;
  ConnectionStream(int fd, const std::atomic<bool>* stop)
      : fd_(fd), stop_(stop) {}
  int fd_ = -1;
  const std::atomic<bool>* stop_ = nullptr;
  uint64_t bytes_ = 0;
};

/// Minimal loopback HTTP/1.1 server: exact-path routing, one request per
/// connection, bounded request sizes and a single accept thread with detached
/// per-connection workers. Thread-safety: routes must be registered before
/// serve(); stop may be called from any thread.
class HttpServer {
 public:
  using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
  using StreamHandler = std::function<void(const HttpRequest&,
                                           ConnectionStream&)>;

  explicit HttpServer(HttpLimits limits = {});
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  Status listen(std::string_view host, uint16_t port);
  void route(std::string_view method, std::string_view path, Handler handler);
  void stream_route(std::string_view path, StreamHandler handler);

  /// Blocking accept loop. Run it on a dedicated thread.
  void serve();
  void request_stop();

  /// Blocks until the accept loop has exited and every connection is done.
  void wait();

  uint16_t port() const { return port_; }
  bool listening() const;
  bool stop_requested() const;

  static bool write_all(int fd, std::string_view data);

 private:
  struct State;
  static void handle_connection_state(const std::shared_ptr<State>& state,
                                      int fd, const std::string& client);

  std::shared_ptr<State> state_;
  std::string host_;
  uint16_t port_ = 0;
};

}  // namespace gbx::ui
