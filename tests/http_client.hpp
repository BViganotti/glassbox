// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "glassbox/json.hpp"

namespace gbx::test {

struct ClientResponse {
  int status = 0;
  std::string raw;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  std::string header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
      if (key == name) return value;
    }
    return {};
  }
};

inline int connect_to_port(uint16_t port) {
  for (int attempt = 0; attempt < 300; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) == 0) {
      struct timeval tv {};
      tv.tv_sec = 5;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      return fd;
    }
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return -1;
}

inline bool send_all(int fd, const std::string& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, 0);
    if (n <= 0) return false;
    offset += static_cast<size_t>(n);
  }
  return true;
}

inline std::string read_all(int fd, size_t limit = 8 * 1024 * 1024) {
  std::string response;
  char buffer[8192];
  while (response.size() < limit) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    response.append(buffer, static_cast<size_t>(n));
  }
  return response;
}

inline ClientResponse parse_response(const std::string& raw) {
  ClientResponse response;
  response.raw = raw;
  const size_t header_end = raw.find("\r\n\r\n");
  const std::string head =
      header_end == std::string::npos ? raw : raw.substr(0, header_end);
  if (header_end != std::string::npos) response.body = raw.substr(header_end + 4);
  const size_t line_end = head.find("\r\n");
  const std::string status_line = head.substr(0, line_end);
  const size_t first_space = status_line.find(' ');
  if (first_space != std::string::npos && status_line.size() > first_space + 3) {
    response.status = std::atoi(status_line.c_str() + first_space + 1);
  }
  size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
  while (pos < head.size()) {
    const size_t end = head.find("\r\n", pos);
    const std::string line =
        head.substr(pos, end == std::string::npos ? head.size() - pos
                                                  : end - pos);
    pos = end == std::string::npos ? head.size() : end + 2;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    response.headers.emplace_back(name, line.substr(colon + 1));
  }
  return response;
}

inline ClientResponse http_request(uint16_t port, const std::string& method,
                                   const std::string& path,
                                   const std::string& body = {},
                                   const std::string& token = {},
                                   const std::string& extra_headers = {},
                                   const std::string& host = {}) {
  const int fd = connect_to_port(port);
  if (fd < 0) return {};
  std::string request = method + " " + path + " HTTP/1.1\r\n";
  request += host.empty()
                 ? "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                 : "Host: " + host + "\r\n";
  if (!token.empty()) request += "X-Glassbox-Token: " + token + "\r\n";
  if (!body.empty() || method == "POST") {
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += extra_headers;
  request += "Connection: close\r\n\r\n";
  request += body;
  if (!send_all(fd, request)) {
    ::close(fd);
    return {};
  }
  const std::string raw = read_all(fd);
  ::close(fd);
  return parse_response(raw);
}

inline std::string sse_read_until(uint16_t port, const std::string& path,
                                  std::string_view needle, size_t max_bytes,
                                  int timeout_ms = 5000) {
  const int fd = connect_to_port(port);
  if (fd < 0) return {};
  std::string request = "GET " + path + " HTTP/1.1\r\n";
  request += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
  request += "Connection: close\r\n\r\n";
  if (!send_all(fd, request)) {
    ::close(fd);
    return {};
  }
  std::string response;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buffer[8192];
  while (response.size() < max_bytes &&
         std::chrono::steady_clock::now() < deadline) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    response.append(buffer, static_cast<size_t>(n));
    if (response.find(needle) != std::string::npos) break;
  }
  ::close(fd);
  return response;
}

inline bool json_ok(const std::string& text, const char* key) {
  gbx::json::Value value;
  if (!gbx::json::parse(text, value).ok()) return false;
  const auto* field = value.find(key);
  return field != nullptr && field->as_boolean(false);
}

}  // namespace gbx::test
