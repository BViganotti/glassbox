// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace gbx::json {
namespace {

constexpr int64_t kMaxSafeInteger = 9007199254740991ll;  // 2^53 - 1

/// Validates one UTF-8 sequence at `pos`, returning its length or 0.
size_t utf8_sequence_length(std::string_view text, size_t pos) {
  const auto first = static_cast<unsigned char>(text[pos]);
  if (first < 0x80) return 1;
  size_t length = 0;
  uint32_t codepoint = 0;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    codepoint = first & 0x1f;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    codepoint = first & 0x0f;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    codepoint = first & 0x07;
  } else {
    return 0;
  }
  if (pos + length > text.size()) return 0;
  for (size_t i = 1; i < length; ++i) {
    const auto byte = static_cast<unsigned char>(text[pos + i]);
    if ((byte & 0xc0) != 0x80) return 0;
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }
  if (length == 2 && codepoint < 0x80) return 0;
  if (length == 3 && codepoint < 0x800) return 0;
  if (length == 4 && codepoint < 0x10000) return 0;
  if (codepoint > 0x10ffff) return 0;
  if (codepoint >= 0xd800 && codepoint <= 0xdfff) return 0;
  return length;
}

void append_utf8(std::string& out, uint32_t codepoint) {
  if (codepoint <= 0x7f) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

}  // namespace

void Writer::clear() {
  out_.clear();
  stack_.clear();
  after_key_ = false;
}

void Writer::separators_ok() const {}

void Writer::before_value() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (stack_.empty()) return;
  auto& level = stack_.back();
  if (level.object) return;  // objects require keys; callers always use key()
  if (!level.first) out_.push_back(',');
  level.first = false;
}

void Writer::begin_object() {
  before_value();
  out_.push_back('{');
  stack_.push_back({true, true});
}

void Writer::end_object() {
  if (stack_.empty() || !stack_.back().object) return;
  stack_.pop_back();
  out_.push_back('}');
  after_key_ = false;
}

void Writer::begin_array() {
  before_value();
  out_.push_back('[');
  stack_.push_back({false, true});
}

void Writer::end_array() {
  if (stack_.empty() || stack_.back().object) return;
  stack_.pop_back();
  out_.push_back(']');
  after_key_ = false;
}

void Writer::key(std::string_view name) {
  if (stack_.empty() || !stack_.back().object) return;
  auto& level = stack_.back();
  if (!level.first) out_.push_back(',');
  level.first = false;
  append_escaped(name);
  out_.push_back(':');
  after_key_ = true;
}

void Writer::append_escaped(std::string_view text) {
  out_.push_back('"');
  size_t i = 0;
  while (i < text.size()) {
    const auto byte = static_cast<unsigned char>(text[i]);
    switch (byte) {
      case '"': out_ += "\\\""; ++i; continue;
      case '\\': out_ += "\\\\"; ++i; continue;
      case '\b': out_ += "\\b"; ++i; continue;
      case '\f': out_ += "\\f"; ++i; continue;
      case '\n': out_ += "\\n"; ++i; continue;
      case '\r': out_ += "\\r"; ++i; continue;
      case '\t': out_ += "\\t"; ++i; continue;
      default: break;
    }
    if (byte < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
      out_ += buffer;
      ++i;
      continue;
    }
    if (byte < 0x80) {
      out_.push_back(static_cast<char>(byte));
      ++i;
      continue;
    }
    const size_t length = utf8_sequence_length(text, i);
    if (length == 0) {
      out_ += "\xef\xbf\xbd";  // U+FFFD
      ++i;
      continue;
    }
    out_.append(text.substr(i, length));
    i += length;
  }
  out_.push_back('"');
}

void Writer::value(std::string_view text) {
  before_value();
  append_escaped(text);
}

void Writer::number(double value) {
  before_value();
  if (!std::isfinite(value)) {
    out_ += "null";
    return;
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  if (std::strchr(buffer, 'e') == nullptr && std::strchr(buffer, 'E') == nullptr &&
      std::strchr(buffer, '.') == nullptr) {
    out_ += buffer;
    out_ += ".0";
  } else {
    out_ += buffer;
  }
}

void Writer::integer(int64_t value) {
  before_value();
  if (value > kMaxSafeInteger || value < -kMaxSafeInteger) {
    append_escaped(std::to_string(value));
    return;
  }
  out_ += std::to_string(value);
}

void Writer::boolean(bool value) {
  before_value();
  out_ += value ? "true" : "false";
}

void Writer::null() {
  before_value();
  out_ += "null";
}

std::string quote(std::string_view text) {
  Writer writer;
  writer.value(text);
  std::string result = writer.take();
  return result;
}

const Value* Value::find(std::string_view key_name) const {
  if (type != Type::Object) return nullptr;
  for (const auto& [name, value] : object) {
    if (name == key_name) return &value;
  }
  return nullptr;
}

namespace {

class Parser {
 public:
  Parser(std::string_view text, const ParseLimits& limits)
      : text_(text), limits_(limits) {}

  Status parse(Value& out) {
    skip_whitespace();
    if (!parse_value(out, 0)) {
      return Status::error(ErrorCode::Corrupt,
                           "JSON error at offset " + std::to_string(pos_) +
                               ": " + error_);
    }
    skip_whitespace();
    if (pos_ != text_.size()) {
      return Status::error(ErrorCode::Corrupt,
                           "JSON trailing data at offset " +
                               std::to_string(pos_));
    }
    return Status::success();
  }

 private:
  bool fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return false;
  }

  void skip_whitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return fail(std::string("expected '") + expected + "'");
  }

  bool parse_value(Value& out, size_t depth) {
    if (depth > limits_.max_depth) return fail("nesting too deep");
    if (pos_ >= text_.size()) return fail("unexpected end of input");
    const char c = text_[pos_];
    if (c == '{') return parse_object(out, depth);
    if (c == '[') return parse_array(out, depth);
    if (c == '"') {
      out.type = Value::Type::String;
      return parse_string(out.string);
    }
    if (c == 't') return parse_literal("true", out, true);
    if (c == 'f') return parse_literal("false", out, false);
    if (c == 'n') return parse_literal("null", out);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    return fail("unexpected character");
  }

  bool parse_literal(std::string_view literal, Value& out) {
    if (text_.substr(pos_, literal.size()) != literal) {
      return fail("invalid literal");
    }
    pos_ += literal.size();
    out.type = Value::Type::Null;
    return true;
  }

  bool parse_literal(std::string_view literal, Value& out, bool boolean) {
    if (!parse_literal(literal, out)) return false;
    out.type = Value::Type::Boolean;
    out.boolean = boolean;
    return true;
  }

  bool parse_number(Value& out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return fail("truncated number");
    if (text_[pos_] == '0') {
      ++pos_;
      if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        return fail("leading zero");
      }
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      return fail("invalid number");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return fail("invalid fraction");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return fail("invalid exponent");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    const std::string number(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (end == number.c_str() || !std::isfinite(value)) {
      return fail("number out of range");
    }
    out.type = Value::Type::Number;
    out.number = value;
    return true;
  }

  bool parse_hex4(uint32_t& out) {
    if (pos_ + 4 > text_.size()) return fail("truncated unicode escape");
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + static_cast<size_t>(i)];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
      else return fail("invalid unicode escape");
    }
    pos_ += 4;
    out = value;
    return true;
  }

  bool parse_string(std::string& out) {
    if (!consume('"')) return false;
    out.clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= text_.size()) return fail("truncated escape");
        const char escaped = text_[pos_++];
        switch (escaped) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t codepoint = 0;
            if (!parse_hex4(codepoint)) return false;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
              if (pos_ + 2 > text_.size() || text_[pos_] != '\\' ||
                  text_[pos_ + 1] != 'u') {
                return fail("unpaired high surrogate");
              }
              pos_ += 2;
              uint32_t low = 0;
              if (!parse_hex4(low)) return false;
              if (low < 0xdc00 || low > 0xdfff) {
                return fail("invalid low surrogate");
              }
              codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                          (low - 0xdc00);
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
              return fail("unpaired low surrogate");
            }
            append_utf8(out, codepoint);
            break;
          }
          default:
            return fail("invalid escape");
        }
      } else {
        out.push_back(c);
      }
      if (out.size() > limits_.max_string) return fail("string too long");
    }
    return fail("unterminated string");
  }

  bool parse_array(Value& out, size_t depth) {
    if (!consume('[')) return false;
    out.type = Value::Type::Array;
    out.array.clear();
    skip_whitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      if (out.array.size() >= limits_.max_array) return fail("array too large");
      Value element;
      skip_whitespace();
      if (!parse_value(element, depth + 1)) return false;
      out.array.push_back(std::move(element));
      skip_whitespace();
      if (pos_ >= text_.size()) return fail("truncated array");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parse_object(Value& out, size_t depth) {
    if (!consume('{')) return false;
    out.type = Value::Type::Object;
    out.object.clear();
    skip_whitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      if (out.object.size() >= limits_.max_object) {
        return fail("object too large");
      }
      skip_whitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return fail("expected object key");
      }
      std::string key;
      if (!parse_string(key)) return false;
      skip_whitespace();
      if (!consume(':')) return false;
      Value value;
      skip_whitespace();
      if (!parse_value(value, depth + 1)) return false;
      out.object.emplace_back(std::move(key), std::move(value));
      skip_whitespace();
      if (pos_ >= text_.size()) return fail("truncated object");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  std::string_view text_;
  ParseLimits limits_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

Status parse(std::string_view text, Value& out, const ParseLimits& limits) {
  out = Value{};
  if (text.size() > limits.max_bytes) {
    return Status::error(ErrorCode::LimitExceeded, "JSON body too large");
  }
  Parser parser(text, limits);
  return parser.parse(out);
}

}  // namespace gbx::json
