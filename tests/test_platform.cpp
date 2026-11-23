// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "platform/platform.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "test_framework.hpp"

TEST(platform_thread_name_roundtrip) {
  gbx::platform::set_current_thread_name("gbx-test-thread");
  CHECK_EQ(gbx::platform::current_thread_name(), std::string("gbx-test-thread"));
}

TEST(platform_random_bytes_are_not_constant) {
  unsigned char first[32] = {};
  unsigned char second[32] = {};
  gbx::platform::random_bytes(first, sizeof(first));
  gbx::platform::random_bytes(second, sizeof(second));
  bool any = false;
  for (unsigned char byte : first) any = any || byte != 0;
  CHECK(any);
  CHECK(std::memcmp(first, second, sizeof(first)) != 0);
}

TEST(platform_browser_command_is_present) {
  const char* command = gbx::platform::browser_command();
  CHECK(command != nullptr);
  CHECK(std::strlen(command) > 0);
}

TEST(platform_send_suppresses_sigpipe) {
  int sockets[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  if (sockets[0] < 0) return;
  gbx::platform::set_no_sigpipe(sockets[0]);
  ::close(sockets[1]);
  errno = 0;
  const long result = gbx::platform::send_bytes(sockets[0], "x", 1);
  CHECK(result < 0);
  CHECK(errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN);
  ::close(sockets[0]);
}
