// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "ui/http.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "glassbox/json.hpp"

namespace gbx::ui {
namespace {

constexpr size_t kMaxRequestLine = 8192;
constexpr size_t kMaxTarget = 4096;
constexpr size_t kMaxResponseBytes = 32 * 1024 * 1024;
constexpr size_t kMaxHeaders = 100;
constexpr size_t kMaxQueryParams = 32;

const char* reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

bool set_socket_timeout(int fd, int milliseconds) {
  struct timeval tv {};
  tv.tv_sec = milliseconds / 1000;
  tv.tv_usec = (milliseconds % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool percent_decode(std::string_view input, std::string& out, bool plus_space) {
  out.clear();
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (c == '%') {
      if (i + 2 >= input.size()) return false;
      const int hi = hex_value(input[i + 1]);
      const int lo = hex_value(input[i + 2]);
      if (hi < 0 || lo < 0) return false;
      const char decoded = static_cast<char>((hi << 4) | lo);
      if (decoded == '\0') return false;
      out.push_back(decoded);
      i += 2;
    } else if (c == '+' && plus_space) {
      out.push_back(' ');
    } else if (c == '\0') {
      return false;
    } else {
      out.push_back(c);
    }
  }
  return true;
}

bool parse_headers(std::string_view block,
                   std::vector<std::pair<std::string, std::string>>& headers) {
  size_t pos = 0;
  while (pos < block.size()) {
    const size_t end = block.find("\r\n", pos);
    const size_t line_end = end == std::string_view::npos ? block.size() : end;
    const std::string_view line = block.substr(pos, line_end - pos);
    pos = end == std::string_view::npos ? block.size() : end + 2;
    if (line.empty()) continue;
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    std::string name;
    name.reserve(colon);
    for (size_t i = 0; i < colon; ++i) {
      const char c = line[i];
      if (c >= 'A' && c <= 'Z') {
        name.push_back(static_cast<char>(c - 'A' + 'a'));
      } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_') {
        name.push_back(c);
      } else {
        return false;
      }
    }
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    if (name.size() > 64 || value.size() > 8192) return false;
    headers.emplace_back(std::move(name), std::string(value));
  }
  return true;
}

bool decode_target(const std::string& target, std::string& path,
                   std::string& query_string, std::string& error) {
  if (target.empty() || target[0] != '/') {
    error = "target must start with /";
    return false;
  }
  const size_t question = target.find('?');
  const std::string_view raw_path =
      question == std::string::npos ? std::string_view(target)
                                    : std::string_view(target).substr(0, question);
  if (!percent_decode(raw_path, path, false)) {
    error = "invalid percent encoding in path";
    return false;
  }
  if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
      path.size() > kMaxTarget) {
    error = "invalid path";
    return false;
  }
  query_string =
      question == std::string::npos ? std::string() : target.substr(question + 1);
  return true;
}

bool parse_query(std::string_view query_string,
                 std::vector<std::pair<std::string, std::string>>& out,
                 std::string& error) {
  size_t pos = 0;
  while (pos <= query_string.size() && !query_string.empty()) {
    const size_t amp = query_string.find('&', pos);
    const size_t end = amp == std::string_view::npos ? query_string.size() : amp;
    const std::string_view pair = query_string.substr(pos, end - pos);
    if (!pair.empty()) {
      if (out.size() >= kMaxQueryParams) {
        error = "too many query parameters";
        return false;
      }
      const size_t eq = pair.find('=');
      const std::string_view raw_key =
          eq == std::string_view::npos ? pair : pair.substr(0, eq);
      const std::string_view raw_value =
          eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
      std::string key;
      std::string value;
      if (!percent_decode(raw_key, key, true) ||
          !percent_decode(raw_value, value, true)) {
        error = "invalid query encoding";
        return false;
      }
      if (key.size() > 128 || value.size() > 4096) {
        error = "query parameter too long";
        return false;
      }
      out.emplace_back(std::move(key), std::move(value));
    }
    if (amp == std::string_view::npos) break;
    pos = end + 1;
  }
  return true;
}

bool read_request(int fd, const HttpLimits& limits, std::string& buffer,
                  size_t& header_end) {
  buffer.clear();
  char chunk[8192];
  while (true) {
    const size_t marker = buffer.find("\r\n\r\n");
    if (marker != std::string::npos) {
      header_end = marker + 4;
      return true;
    }
    if (buffer.size() >= limits.max_header_bytes) return false;
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    buffer.append(chunk, static_cast<size_t>(n));
    if (buffer.size() > limits.max_header_bytes + limits.max_body_bytes) {
      return false;
    }
  }
}

void send_status(int fd, int status, std::string_view message) {
  json::Writer writer;
  writer.begin_object();
  writer.key_integer("status", status);
  writer.key_string("error", message);
  writer.end_object();
  const std::string body = writer.str();
  std::string response = "HTTP/1.1 " + std::to_string(status) + " " +
                         reason_phrase(status) + "\r\n";
  response += "Content-Type: application/json; charset=utf-8\r\n";
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  response += "Connection: close\r\n";
  response += "Cache-Control: no-store\r\n";
  response += "\r\n";
  response += body;
  HttpServer::write_all(fd, response);
}

std::string join_key(std::string_view method, std::string_view path) {
  std::string key(method);
  key.push_back(' ');
  key.append(path);
  return key;
}

}  // namespace

std::string HttpRequest::header(std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (key == name) return value;
  }
  return {};
}

std::string HttpRequest::query_value(std::string_view name) const {
  for (const auto& [key, value] : query) {
    if (key == name) return value;
  }
  return {};
}

void HttpResponse::set_body(std::string text, std::string_view type) {
  body = std::move(text);
  if (!type.empty()) content_type = std::string(type);
}

void HttpResponse::set_error(int code, std::string message) {
  status = code;
  content_type = "application/json; charset=utf-8";
  json::Writer writer;
  writer.begin_object();
  writer.key_integer("status", code);
  writer.key_string("error", message);
  writer.end_object();
  body = writer.str();
}

bool ConnectionStream::alive() const {
  return fd_ >= 0 && stop_ != nullptr && !stop_->load();
}

bool ConnectionStream::write(std::string_view data) {
  if (!alive()) return false;
  if (!HttpServer::write_all(fd_, data)) return false;
  bytes_ += data.size();
  return true;
}

bool ConnectionStream::sse_event(std::string_view event, std::string_view data) {
  std::string frame;
  frame.reserve(event.size() + data.size() + 16);
  frame += "event: ";
  frame += event;
  frame += '\n';
  size_t pos = 0;
  while (pos <= data.size()) {
    const size_t newline = data.find('\n', pos);
    const size_t end = newline == std::string_view::npos ? data.size() : newline;
    frame += "data: ";
    frame.append(data.substr(pos, end - pos));
    frame += '\n';
    if (newline == std::string_view::npos) break;
    pos = end + 1;
  }
  frame += '\n';
  return write(frame);
}

bool ConnectionStream::comment(std::string_view text) {
  std::string frame = ": ";
  frame += text;
  frame += "\n\n";
  return write(frame);
}

struct HttpServer::State {
  HttpLimits limits;
  std::atomic<bool> stop{false};
  std::atomic<bool> listening{false};
  std::atomic<int> active{0};
  int listen_fd = -1;
  std::mutex route_mu;
  std::map<std::string, Handler> routes;
  std::map<std::string, StreamHandler> streams;
  std::mutex wait_mu;
  std::condition_variable wait_cv;
};

HttpServer::HttpServer(HttpLimits limits) : state_(std::make_shared<State>()) {
  state_->limits = limits;
}

HttpServer::~HttpServer() {
  request_stop();
  wait();
}

Status HttpServer::listen(std::string_view host, uint16_t port) {
  if (state_->listen_fd >= 0) {
    return Status::error(ErrorCode::Busy, "server already listening");
  }
  if (host != "127.0.0.1" && host != "localhost") {
    return Status::error(ErrorCode::InvalidArgument,
                         "only loopback binding is supported");
  }
  host_ = std::string(host);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return Status::error(ErrorCode::IoError,
                         std::string("socket: ") + std::strerror(errno));
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    const auto status =
        Status::error(ErrorCode::IoError,
                      std::string("bind: ") + std::strerror(errno));
    ::close(fd);
    return status;
  }
  if (::listen(fd, 64) != 0) {
    const auto status =
        Status::error(ErrorCode::IoError,
                      std::string("listen: ") + std::strerror(errno));
    ::close(fd);
    return status;
  }
  struct sockaddr_in bound {};
  socklen_t length = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &length) != 0) {
    ::close(fd);
    return Status::error(ErrorCode::IoError, "getsockname failed");
  }
  port_ = ntohs(bound.sin_port);
  state_->listen_fd = fd;
  state_->listening.store(true);
  return Status::success();
}

void HttpServer::route(std::string_view method, std::string_view path,
                       Handler handler) {
  std::lock_guard lock(state_->route_mu);
  state_->routes[join_key(method, path)] = std::move(handler);
}

void HttpServer::stream_route(std::string_view path, StreamHandler handler) {
  std::lock_guard lock(state_->route_mu);
  state_->streams[std::string(path)] = std::move(handler);
}

bool HttpServer::listening() const { return state_->listening.load(); }

bool HttpServer::stop_requested() const { return state_->stop.load(); }

void HttpServer::request_stop() {
  state_->stop.store(true);
  if (state_->listen_fd >= 0) {
    ::shutdown(state_->listen_fd, SHUT_RDWR);
  }
}

void HttpServer::wait() {
  std::unique_lock lock(state_->wait_mu);
  state_->wait_cv.wait_for(lock, std::chrono::seconds(5), [this] {
    return state_->active.load() == 0;
  });
}

bool HttpServer::write_all(int fd, std::string_view data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n =
        ::send(fd, data.data() + offset, data.size() - offset, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    offset += static_cast<size_t>(n);
  }
  return true;
}

void HttpServer::serve() {
  if (state_->listen_fd < 0) return;
  const int listen_fd = state_->listen_fd;
  while (!state_->stop.load()) {
    struct pollfd poll_fd {};
    poll_fd.fd = listen_fd;
    poll_fd.events = POLLIN;
    const int ready = ::poll(&poll_fd, 1, 200);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;
    struct sockaddr_in client {};
    socklen_t client_length = sizeof(client);
    const int fd = ::accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client),
                            &client_length);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      if (state_->stop.load()) break;
      continue;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    set_socket_timeout(fd, state_->limits.io_timeout_ms);
    const int active = state_->active.fetch_add(1) + 1;
    if (active > state_->limits.max_connections) {
      state_->active.fetch_sub(1);
      send_status(fd, 503, "too many connections");
      ::close(fd);
      continue;
    }
    std::shared_ptr<State> state = state_;
    std::thread([state, fd] {
      struct sockaddr_in peer {};
      socklen_t peer_length = sizeof(peer);
      std::string client;
      if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&peer),
                        &peer_length) == 0) {
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text)) != nullptr) {
          client = text;
        }
      }
      handle_connection_state(state, fd, client);
      ::close(fd);
      {
        std::lock_guard lock(state->wait_mu);
        state->active.fetch_sub(1);
      }
      state->wait_cv.notify_all();
    }).detach();
  }
  state_->listening.store(false);
}

void HttpServer::handle_connection_state(const std::shared_ptr<State>& state,
                                        int fd, const std::string& client) {
  std::string buffer;
  size_t header_end = 0;
  if (!read_request(fd, state->limits, buffer, header_end)) {
    send_status(fd, 400, "malformed or oversized request headers");
    return;
  }
  const std::string_view header_block(buffer.data(), header_end);
  const size_t line_end = header_block.find("\r\n");
  if (line_end == std::string_view::npos || line_end > kMaxRequestLine) {
    send_status(fd, 400, "invalid request line");
    return;
  }
  const std::string_view request_line = header_block.substr(0, line_end);
  const size_t first_space = request_line.find(' ');
  const size_t second_space =
      first_space == std::string_view::npos
          ? std::string_view::npos
          : request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos ||
      second_space == std::string_view::npos) {
    send_status(fd, 400, "invalid request line");
    return;
  }
  HttpRequest request;
  request.method = std::string(request_line.substr(0, first_space));
  request.target = std::string(
      request_line.substr(first_space + 1, second_space - first_space - 1));
  const std::string_view version = request_line.substr(second_space + 1);
  if (request.method != "GET" && request.method != "POST" &&
      request.method != "HEAD") {
    send_status(fd, 405, "method not allowed");
    return;
  }
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    send_status(fd, 400, "unsupported HTTP version");
    return;
  }
  if (!parse_headers(header_block.substr(line_end + 2),
                     request.headers)) {
    send_status(fd, 400, "invalid headers");
    return;
  }
  if (request.headers.size() > kMaxHeaders) {
    send_status(fd, 431, "too many headers");
    return;
  }
  std::string target_error;
  if (!decode_target(request.target, request.path, request.query_string,
                     target_error)) {
    send_status(fd, 400, target_error);
    return;
  }
  if (!parse_query(request.query_string, request.query, target_error)) {
    send_status(fd, 400, target_error);
    return;
  }
  request.client = client;

  std::string transfer_encoding = request.header("transfer-encoding");
  if (!transfer_encoding.empty()) {
    send_status(fd, 501, "transfer encoding is not supported");
    return;
  }
  size_t content_length = 0;
  bool saw_length = false;
  for (const auto& [name, value] : request.headers) {
    if (name != "content-length") continue;
    if (saw_length) {
      send_status(fd, 400, "duplicate content-length");
      return;
    }
    saw_length = true;
    if (value.empty() || value.size() > 18) {
      send_status(fd, 400, "invalid content-length");
      return;
    }
    for (char c : value) {
      if (c < '0' || c > '9') {
        send_status(fd, 400, "invalid content-length");
        return;
      }
      content_length = content_length * 10 + static_cast<size_t>(c - '0');
    }
  }
  if (content_length > state->limits.max_body_bytes) {
    send_status(fd, 413, "request body too large");
    return;
  }
  const size_t buffered_body = buffer.size() - header_end;
  if (buffered_body < content_length) {
    const size_t missing = content_length - buffered_body;
    std::string body(missing, '\0');
    size_t read_total = 0;
    while (read_total < missing) {
      const ssize_t n = ::recv(fd, body.data() + read_total, missing - read_total, 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        send_status(fd, 400, "truncated request body");
        return;
      }
      if (n == 0) {
        send_status(fd, 400, "truncated request body");
        return;
      }
      read_total += static_cast<size_t>(n);
    }
    request.body.assign(buffer.data() + header_end, buffered_body);
    request.body += body;
  } else {
    request.body.assign(buffer.data() + header_end, content_length);
  }

  // Routing under a short lock; handlers run without it.
  Handler handler;
  StreamHandler stream;
  {
    std::lock_guard lock(state->route_mu);
    auto route_it =
        state->routes.find(join_key(request.method, request.path));
    if (route_it != state->routes.end()) {
      handler = route_it->second;
    } else if (request.method == "HEAD") {
      auto head_it = state->routes.find(join_key("GET", request.path));
      if (head_it != state->routes.end()) handler = head_it->second;
    }
    if (request.method == "GET") {
      auto stream_it = state->streams.find(request.path);
      if (stream_it != state->streams.end()) stream = stream_it->second;
    }
  }

  if (stream) {
    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Connection: close\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    if (!write_all(fd, header)) return;
    ConnectionStream connection(fd, &state->stop);
    try {
      stream(request, connection);
    } catch (...) {
      // A stream failure just closes the connection.
    }
    return;
  }

  if (!handler) {
    send_status(fd, 404, "not found");
    return;
  }
  HttpResponse response;
  try {
    handler(request, response);
  } catch (const std::exception& e) {
    response = HttpResponse{};
    response.set_error(500, std::string("handler failure: ") + e.what());
  } catch (...) {
    response = HttpResponse{};
    response.set_error(500, "handler failure");
  }
  if (response.body.size() > kMaxResponseBytes) {
    response = HttpResponse{};
    response.set_error(500, "response exceeds size limit");
  }
  std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
                     reason_phrase(response.status) + "\r\n";
  head += "Content-Type: " + response.content_type + "\r\n";
  head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  head += "Connection: close\r\n";
  head += "Cache-Control: no-store\r\n";
  head += "X-Content-Type-Options: nosniff\r\n";
  head += "Referrer-Policy: no-referrer\r\n";
  for (const auto& [name, value] : response.headers) {
    if (name.find('\r') != std::string::npos ||
        name.find('\n') != std::string::npos ||
        value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos) {
      continue;
    }
    head += name;
    head += ": ";
    head += value;
    head += "\r\n";
  }
  head += "\r\n";
  if (!write_all(fd, head)) return;
  if (request.method != "HEAD" && !response.body.empty()) {
    write_all(fd, response.body);
  }
}

}  // namespace gbx::ui
