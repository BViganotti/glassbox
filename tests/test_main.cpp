// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace gbx::test {
namespace {

bool g_failed_any = false;
int g_failures = 0;

}  // namespace

unsigned long long checks = 0;

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

bool register_test(const char* name, void (*fn)()) {
  registry().push_back({name, fn});
  return true;
}

void fail(const char* file, int line, std::string_view message) {
  ++g_failures;
  g_failed_any = true;
  std::fprintf(stderr, "  FAIL %s:%d: %.*s\n", file, line,
               static_cast<int>(message.size()), message.data());
}

void fail_value(const char* file, int line, const char* lhs_expr,
                const char* rhs_expr, const std::string& lhs,
                const std::string& rhs) {
  ++g_failures;
  g_failed_any = true;
  std::fprintf(stderr, "  FAIL %s:%d: %s != %s (%s vs %s)\n", file, line,
               lhs_expr, rhs_expr, lhs.c_str(), rhs.c_str());
}

}  // namespace gbx::test

int main(int argc, char** argv) {
  using namespace gbx::test;
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const auto& test : registry()) {
    if (filter && std::strstr(test.name, filter) == nullptr) continue;
    const int before = 0;
    (void)before;
    std::fprintf(stderr, "[ RUN  ] %s\n", test.name);
    try {
      test.fn();
    } catch (const std::exception& e) {
      fail(__FILE__, __LINE__,
           std::string("uncaught exception: ") + e.what());
    } catch (...) {
      fail(__FILE__, __LINE__, "uncaught non-standard exception");
    }
    ++ran;
  }
  if (ran == 0) {
    std::fprintf(stderr, "no tests matched\n");
    return 2;
  }
  std::fprintf(stderr, "[ DONE ] %d tests, %llu checks, %d failures\n", ran,
               static_cast<unsigned long long>(checks), g_failures);
  return g_failures == 0 ? 0 : 1;
}
