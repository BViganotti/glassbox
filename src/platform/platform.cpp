// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "platform/platform.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

namespace gbx::platform {
namespace {

/// Linux limits thread names to 15 bytes plus NUL.
std::string limit_thread_name(std::string_view name) {
  constexpr size_t kLimit = 15;
  std::string out(name.substr(0, kLimit));
  return out;
}

}  // namespace

void set_current_thread_name(std::string_view name) {
  const std::string text = limit_thread_name(name);
#if defined(__APPLE__)
  pthread_setname_np(text.c_str());
#else
  pthread_setname_np(pthread_self(), text.c_str());
#endif
}

std::string current_thread_name() {
  char buffer[64] = {};
#if defined(__APPLE__)
  if (pthread_getname_np(pthread_self(), buffer, sizeof(buffer)) != 0) {
    return {};
  }
#else
  if (pthread_getname_np(pthread_self(), buffer, sizeof(buffer)) != 0) {
    return {};
  }
#endif
  return std::string(buffer);
}

void random_bytes(unsigned char* bytes, size_t size) {
#if defined(__APPLE__)
  arc4random_buf(bytes, size);
#else
  size_t filled = 0;
  while (filled < size) {
    const ssize_t n = ::getrandom(bytes + filled, size - filled, 0);
    if (n > 0) {
      filled += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  if (filled == size) return;
  // Fallback: /dev/urandom. If that also fails the buffer stays zero-filled.
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::memset(bytes, 0, size);
    return;
  }
  filled = 0;
  while (filled < size) {
    const ssize_t n = ::read(fd, bytes + filled, size - filled);
    if (n > 0) {
      filled += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  ::close(fd);
  if (filled < size) std::memset(bytes, 0, size);
#endif
}

void set_no_sigpipe(int fd) {
#if defined(__APPLE__)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;  // MSG_NOSIGNAL is applied per send instead.
#endif
}

long send_bytes(int fd, const void* data, size_t size) {
#if defined(__APPLE__)
  return ::send(fd, data, size, 0);
#else
  return ::send(fd, data, size, MSG_NOSIGNAL);
#endif
}

const char* browser_command() {
#if defined(__APPLE__)
  return "open";
#else
  return "xdg-open";
#endif
}

}  // namespace gbx::platform
