// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace gbx::test {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();
bool register_test(const char* name, void (*fn)());

/// Records a failure, printing a diagnostic with file/line.
void fail(const char* file, int line, std::string_view message);

/// Incremented by every check; used to report total assertion volume.
extern unsigned long long checks;

#define GBX_TEST_CONCAT2(a, b) a##b
#define GBX_TEST_CONCAT(a, b) GBX_TEST_CONCAT2(a, b)

#define TEST(name)                                                       \
  static void name();                                                    \
  [[maybe_unused]] static const bool GBX_TEST_CONCAT(                    \
      reg_, GBX_TEST_CONCAT(name, __LINE__)) =                           \
      ::gbx::test::register_test(#name, &name);                          \
  static void name()

#define CHECK(cond)                                                      \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    if (!(cond)) {                                                       \
      ::gbx::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");         \
    }                                                                    \
  } while (0)

#define CHECK_MSG(cond, msg)                                             \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    if (!(cond)) {                                                       \
      ::gbx::test::fail(__FILE__, __LINE__,                              \
                        std::string("CHECK(" #cond "): ") + (msg));      \
    }                                                                    \
  } while (0)

#define CHECK_EQ(a, b)                                                   \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    auto gbx_a_ = (a);                                                   \
    auto gbx_b_ = (b);                                                   \
    if (!(gbx_a_ == gbx_b_)) {                                           \
      ::gbx::test::fail_value(__FILE__, __LINE__, #a, #b,                \
                              ::gbx::test::render(gbx_a_),               \
                              ::gbx::test::render(gbx_b_));              \
    }                                                                    \
  } while (0)

#define CHECK_NE(a, b)                                                   \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    auto gbx_a_ = (a);                                                   \
    auto gbx_b_ = (b);                                                   \
    if (!(gbx_a_ != gbx_b_)) {                                           \
      ::gbx::test::fail_value(__FILE__, __LINE__, #a, #b,                \
                              ::gbx::test::render(gbx_a_),               \
                              ::gbx::test::render(gbx_b_));              \
    }                                                                    \
  } while (0)

#define CHECK_GT(a, b)                                                   \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    auto gbx_a_ = (a);                                                   \
    auto gbx_b_ = (b);                                                   \
    if (!(gbx_a_ > gbx_b_)) {                                            \
      ::gbx::test::fail_value(__FILE__, __LINE__, #a " > " #b, "",       \
                              ::gbx::test::render(gbx_a_),               \
                              ::gbx::test::render(gbx_b_));              \
    }                                                                    \
  } while (0)

#define CHECK_LT(a, b)                                                   \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    auto gbx_a_ = (a);                                                   \
    auto gbx_b_ = (b);                                                   \
    if (!(gbx_a_ < gbx_b_)) {                                            \
      ::gbx::test::fail_value(__FILE__, __LINE__, #a " < " #b, "",       \
                              ::gbx::test::render(gbx_a_),               \
                              ::gbx::test::render(gbx_b_));              \
    }                                                                    \
  } while (0)

#define CHECK_GE(a, b)                                                   \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    auto gbx_a_ = (a);                                                   \
    auto gbx_b_ = (b);                                                   \
    if (!(gbx_a_ >= gbx_b_)) {                                           \
      ::gbx::test::fail_value(__FILE__, __LINE__, #a " >= " #b, "",      \
                              ::gbx::test::render(gbx_a_),               \
                              ::gbx::test::render(gbx_b_));              \
    }                                                                    \
  } while (0)

#define CHECK_LE(a, b)                                                   \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    auto gbx_a_ = (a);                                                   \
    auto gbx_b_ = (b);                                                   \
    if (!(gbx_a_ <= gbx_b_)) {                                           \
      ::gbx::test::fail_value(__FILE__, __LINE__, #a " <= " #b, "",      \
                              ::gbx::test::render(gbx_a_),               \
                              ::gbx::test::render(gbx_b_));              \
    }                                                                    \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                            \
  do {                                                                   \
    ++::gbx::test::checks;                                               \
    double gbx_a_ = static_cast<double>(a);                              \
    double gbx_b_ = static_cast<double>(b);                              \
    double gbx_d_ = gbx_a_ - gbx_b_;                                     \
    if (gbx_d_ < 0) gbx_d_ = -gbx_d_;                                    \
    if (!(gbx_d_ <= (eps))) {                                            \
      ::gbx::test::fail_value(__FILE__, __LINE__, #a, #b,                \
                              ::gbx::test::render(gbx_a_),               \
                              ::gbx::test::render(gbx_b_));              \
    }                                                                    \
  } while (0)

void fail_value(const char* file, int line, const char* lhs_expr,
                const char* rhs_expr, const std::string& lhs,
                const std::string& rhs);

template <typename T>
std::string render(const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    return "<value>";
  }
}

}  // namespace gbx::test
