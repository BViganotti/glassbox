// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "glassbox/status.hpp"

namespace gbx::json {

/// Streaming JSON writer. Always produces valid JSON: strings are escaped
/// (invalid UTF-8 bytes become U+FFFD), non-finite numbers become null, and
/// integers outside the IEEE-754 safe range are emitted as strings.
class Writer {
 public:
  void begin_object();
  void end_object();
  void begin_array();
  void end_array();
  void key(std::string_view name);
  void value(std::string_view text);
  void value(const char* text) { value(std::string_view(text)); }
  void number(double value);
  void integer(int64_t value);
  void boolean(bool value);
  void null();

  /// Convenience helpers.
  void key_string(std::string_view name, std::string_view text) {
    key(name);
    value(text);
  }
  void key_number(std::string_view name, double value) {
    key(name);
    number(value);
  }
  void key_integer(std::string_view name, int64_t value) {
    key(name);
    integer(value);
  }
  void key_boolean(std::string_view name, bool value) {
    key(name);
    boolean(value);
  }
  void key_null(std::string_view name) {
    key(name);
    null();
  }

  const std::string& str() const { return out_; }
  std::string take() { return std::move(out_); }
  void clear();
  bool empty() const { return out_.empty(); }

 private:
  struct Level {
    bool object = false;
    bool first = true;
  };
  void before_value();
  void separators_ok() const;
  void append_escaped(std::string_view text);

  std::string out_;
  std::vector<Level> stack_;
  bool after_key_ = false;
};

/// Escape a string into a JSON string literal (including quotes).
std::string quote(std::string_view text);

/// A small JSON tree used for request bodies. Parsing is bounded by depth,
/// input size and string size and never throws.
struct Value {
  enum class Type { Null, Boolean, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  bool is_null() const { return type == Type::Null; }
  bool is_object() const { return type == Type::Object; }
  bool is_array() const { return type == Type::Array; }
  bool is_string() const { return type == Type::String; }
  bool is_number() const { return type == Type::Number; }
  bool is_boolean() const { return type == Type::Boolean; }

  const Value* find(std::string_view key) const;
  std::string as_string(std::string_view fallback = {}) const {
    return type == Type::String ? string : std::string(fallback);
  }
  double as_number(double fallback = 0) const {
    return type == Type::Number ? number : fallback;
  }
  bool as_boolean(bool fallback = false) const {
    return type == Type::Boolean ? boolean : fallback;
  }
};

struct ParseLimits {
  size_t max_bytes = 256 * 1024;
  size_t max_depth = 32;
  size_t max_string = 16 * 1024;
  size_t max_array = 10000;
  size_t max_object = 1000;
};

/// Parses a complete JSON document. Trailing non-whitespace is an error.
Status parse(std::string_view text, Value& out,
             const ParseLimits& limits = {});

}  // namespace gbx::json
