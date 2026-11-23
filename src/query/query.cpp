// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/query.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gbx {
namespace {

constexpr size_t kMaxQueryLength = 4096;
constexpr size_t kMaxTokens = 256;
constexpr size_t kMaxLiteral = 512;
constexpr int kMaxDepth = 32;

char lower_ascii(char c) {
  const auto uc = static_cast<unsigned char>(c);
  return static_cast<char>(std::tolower(uc));
}

bool icontains(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool equal = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (lower_ascii(haystack[i + j]) != lower_ascii(needle[j])) {
        equal = false;
        break;
      }
    }
    if (equal) return true;
  }
  return false;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  }
  return true;
}

enum class TokKind {
  End,
  Word,
  Number,
  String,
  LParen,
  RParen,
  And,
  Or,
  Not,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  Contains,
  NotContains,
  Colon,
  Invalid,
};

struct Token {
  TokKind kind = TokKind::End;
  std::string text;
  double number = 0.0;
  size_t offset = 0;
};

bool is_word_char(char c) {
  const auto uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) != 0 || c == '_' || c == '.' || c == '/' || c == '$' ||
         c == '@' || c == '-' || c == '+';
}

bool is_digit(char c) {
  return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {}

  bool lex(std::vector<Token>& out, QueryError& error) {
    out.clear();
    size_t i = 0;
    while (i < source_.size()) {
      const char c = source_[i];
      if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        ++i;
        continue;
      }
      if (out.size() >= kMaxTokens) {
        return fail(i, "query has too many tokens", error);
      }
      Token token;
      token.offset = i;
      if (c == '(' || c == ')') {
        token.kind = c == '(' ? TokKind::LParen : TokKind::RParen;
        token.text = std::string(1, c);
        ++i;
        out.push_back(std::move(token));
        continue;
      }
      if (c == '&' || c == '|') {
        if (i + 1 < source_.size() && source_[i + 1] == c) {
          token.kind = c == '&' ? TokKind::And : TokKind::Or;
          token.text = std::string(2, c);
          i += 2;
          out.push_back(std::move(token));
          continue;
        }
        return fail(i, "expected '&&' or '||'", error);
      }
      if (c == '!' || c == '~' || c == '=' || c == '<' || c == '>') {
        const bool has_next = i + 1 < source_.size();
        const char next = has_next ? source_[i + 1] : '\0';
        if (c == '!' && next == '=') {
          token.kind = TokKind::Ne;
          token.text = "!=";
          i += 2;
        } else if (c == '!' && next == '~') {
          token.kind = TokKind::NotContains;
          token.text = "!~";
          i += 2;
        } else if (c == '=' && next == '=') {
          token.kind = TokKind::Eq;
          token.text = "==";
          i += 2;
        } else if (c == '<' && next == '=') {
          token.kind = TokKind::Le;
          token.text = "<=";
          i += 2;
        } else if (c == '>' && next == '=') {
          token.kind = TokKind::Ge;
          token.text = ">=";
          i += 2;
        } else if (c == '=') {
          token.kind = TokKind::Eq;
          token.text = "=";
          ++i;
        } else if (c == '<') {
          token.kind = TokKind::Lt;
          token.text = "<";
          ++i;
        } else if (c == '>') {
          token.kind = TokKind::Gt;
          token.text = ">";
          ++i;
        } else if (c == '~') {
          token.kind = TokKind::Contains;
          token.text = "~";
          ++i;
        } else {
          token.kind = TokKind::Not;
          token.text = "!";
          ++i;
        }
        out.push_back(std::move(token));
        continue;
      }
      if (c == ':') {
        token.kind = TokKind::Colon;
        token.text = ":";
        ++i;
        out.push_back(std::move(token));
        continue;
      }
      if (c == '"' || c == '\'') {
        const char quote = c;
        ++i;
        std::string value;
        bool closed = false;
        while (i < source_.size()) {
          const char ch = source_[i++];
          if (ch == quote) {
            closed = true;
            break;
          }
          if (ch == '\\' && i < source_.size()) {
            const char escaped = source_[i++];
            switch (escaped) {
              case 'n': value.push_back('\n'); break;
              case 't': value.push_back('\t'); break;
              case 'r': value.push_back('\r'); break;
              case '\\': value.push_back('\\'); break;
              case '"': value.push_back('"'); break;
              case '\'': value.push_back('\''); break;
              default: value.push_back(escaped); break;
            }
          } else {
            value.push_back(ch);
          }
          if (value.size() > kMaxLiteral) {
            return fail(i, "string literal too long", error);
          }
        }
        if (!closed) return fail(token.offset, "unterminated string", error);
        token.kind = TokKind::String;
        token.text = std::move(value);
        out.push_back(std::move(token));
        continue;
      }
      if (is_digit(c) || (c == '-' && i + 1 < source_.size() &&
                          is_digit(source_[i + 1]))) {
        const size_t start = i;
        if (source_[i] == '-') ++i;
        while (i < source_.size() && is_digit(source_[i])) ++i;
        if (i < source_.size() && source_[i] == '.' &&
            i + 1 < source_.size() && is_digit(source_[i + 1])) {
          ++i;
          while (i < source_.size() && is_digit(source_[i])) ++i;
        }
        std::string number_text(source_.substr(start, i - start));
        std::string suffix;
        while (i < source_.size() &&
               std::isalpha(static_cast<unsigned char>(source_[i])) != 0) {
          suffix.push_back(static_cast<char>(
              std::tolower(static_cast<unsigned char>(source_[i]))));
          ++i;
        }
        double number = 0;
        errno = 0;
        char* end = nullptr;
        const std::string text_copy = number_text;
        number = std::strtod(text_copy.c_str(), &end);
        if (end == text_copy.c_str() || errno == ERANGE) {
          return fail(token.offset, "invalid number", error);
        }
        double scale = 1.0;
        if (!suffix.empty()) {
          if (suffix == "ns") scale = 1.0;
          else if (suffix == "us") scale = 1e3;
          else if (suffix == "ms") scale = 1e6;
          else if (suffix == "s") scale = 1e9;
          else if (suffix == "m") scale = 60e9;
          else return fail(token.offset, "unknown duration unit", error);
        }
        if (number > 1e18 || number < -1e18) {
          return fail(token.offset, "number out of range", error);
        }
        token.kind = TokKind::Number;
        token.number = number * scale;
        token.text = std::move(number_text) + suffix;
        out.push_back(std::move(token));
        continue;
      }
      if (is_word_char(c) || c == '\\') {
        const size_t start = i;
        while (i < source_.size() && (is_word_char(source_[i]) || source_[i] == '\\')) {
          ++i;
        }
        std::string word(source_.substr(start, i - start));
        if (word.size() > kMaxLiteral) {
          return fail(start, "word too long", error);
        }
        if (iequals(word, "and")) {
          token.kind = TokKind::And;
        } else if (iequals(word, "or")) {
          token.kind = TokKind::Or;
        } else if (iequals(word, "not")) {
          token.kind = TokKind::Not;
        } else {
          token.kind = TokKind::Word;
        }
        token.text = std::move(word);
        out.push_back(std::move(token));
        continue;
      }
      return fail(i, "unexpected character", error);
    }
    Token end;
    end.kind = TokKind::End;
    end.offset = source_.size();
    out.push_back(std::move(end));
    return true;
  }

 private:
  bool fail(size_t offset, std::string message, QueryError& error) {
    error.offset = offset;
    error.message = std::move(message);
    return false;
  }

  std::string_view source_;
};

enum class NodeKind { And, Or, Not, Compare, Text };

struct ParsedValue {
  enum class Kind { Number, String, Bool } kind = Kind::Number;
  double number = 0;
  std::string text;
  bool boolean = false;
};

bool parse_value_kind(const Token& token, ParsedValue& value) {
  switch (token.kind) {
    case TokKind::Number:
      value.kind = ParsedValue::Kind::Number;
      value.number = token.number;
      return true;
    case TokKind::String:
      value.kind = ParsedValue::Kind::String;
      value.text = token.text;
      return true;
    case TokKind::Word:
      value.kind = ParsedValue::Kind::String;
      value.text = token.text;
      if (iequals(token.text, "true") || iequals(token.text, "false")) {
        value.kind = ParsedValue::Kind::Bool;
        value.boolean = iequals(token.text, "true");
      }
      return true;
    default:
      return false;
  }
}

}  // namespace

struct Query::Node {
  NodeKind kind = NodeKind::Text;
  std::vector<std::shared_ptr<const Node>> children;
  std::string field;
  std::string text;
  TokKind op = TokKind::Eq;
  bool has_value = false;
  ParsedValue value;
  size_t count = 1;
};

namespace {

using NodePtr = std::shared_ptr<const Query::Node>;
using MutableNodePtr = std::shared_ptr<Query::Node>;

struct NodeBuilder {
  static MutableNodePtr make(NodeKind kind) {
    auto node = std::make_shared<Query::Node>();
    node->kind = kind;
    return node;
  }
};

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::string_view source)
      : tokens_(std::move(tokens)), source_(source) {}

  NodePtr parse(QueryError& error) {
    NodePtr node = parse_or(error);
    if (!node) return nullptr;
    if (peek().kind != TokKind::End) {
      fail(peek().offset, "unexpected token after expression", error);
      return nullptr;
    }
    return node;
  }

 private:
  const Token& peek() const { return tokens_[std::min(position_, tokens_.size() - 1)]; }
  const Token& advance() {
    const Token& token = peek();
    if (position_ + 1 < tokens_.size()) ++position_;
    return token;
  }
  bool accept(TokKind kind) {
    if (peek().kind != kind) return false;
    advance();
    return true;
  }
  void fail(size_t offset, std::string message, QueryError& error) {
    error.offset = offset;
    error.message = std::move(message);
  }

  NodePtr parse_or(QueryError& error) {
    if (depth_ >= kMaxDepth) {
      fail(peek().offset, "expression nesting too deep", error);
      return nullptr;
    }
    ++depth_;
    NodePtr left = parse_and(error);
    while (left && accept(TokKind::Or)) {
      NodePtr right = parse_and(error);
      if (!right) return nullptr;
      auto node = NodeBuilder::make(NodeKind::Or);
      node->children = {left, right};
      node->count = left->count + right->count + 1;
      left = node;
    }
    --depth_;
    return left;
  }

  NodePtr parse_and(QueryError& error) {
    NodePtr left = parse_unary(error);
    while (left && accept(TokKind::And)) {
      NodePtr right = parse_unary(error);
      if (!right) return nullptr;
      auto node = NodeBuilder::make(NodeKind::And);
      node->children = {left, right};
      node->count = left->count + right->count + 1;
      left = node;
    }
    return left;
  }

  NodePtr parse_unary(QueryError& error) {
    if (accept(TokKind::Not)) {
      if (depth_ >= kMaxDepth) {
        fail(peek().offset, "expression nesting too deep", error);
        return nullptr;
      }
      ++depth_;
      NodePtr inner = parse_unary(error);
      --depth_;
      if (!inner) return nullptr;
      auto node = NodeBuilder::make(NodeKind::Not);
      node->children = {inner};
      node->count = inner->count + 1;
      return node;
    }
    return parse_primary(error);
  }

  NodePtr parse_primary(QueryError& error) {
    if (accept(TokKind::LParen)) {
      if (depth_ >= kMaxDepth) {
        fail(peek().offset, "expression nesting too deep", error);
        return nullptr;
      }
      ++depth_;
      NodePtr inner = parse_or(error);
      --depth_;
      if (!inner) return nullptr;
      if (!accept(TokKind::RParen)) {
        fail(peek().offset, "missing ')'", error);
        return nullptr;
      }
      return inner;
    }
    const Token& token = peek();
    if (token.kind == TokKind::String) {
      advance();
      auto node = NodeBuilder::make(NodeKind::Text);
      node->text = token.text;
      return node;
    }
    if (token.kind == TokKind::Number) {
      advance();
      auto node = NodeBuilder::make(NodeKind::Text);
      node->text = token.text;
      return node;
    }
    if (token.kind != TokKind::Word) {
      fail(token.offset, "expected a field or expression", error);
      return nullptr;
    }
    advance();
    const TokKind op = peek().kind;
    const bool is_op = op == TokKind::Eq || op == TokKind::Ne ||
                       op == TokKind::Lt || op == TokKind::Le ||
                       op == TokKind::Gt || op == TokKind::Ge ||
                       op == TokKind::Contains || op == TokKind::NotContains ||
                       op == TokKind::Colon;
    if (!is_op) {
      auto node = NodeBuilder::make(NodeKind::Text);
      node->text = token.text;
      return node;
    }
    advance();
    ParsedValue value;
    if (!parse_value_kind(peek(), value)) {
      fail(peek().offset, "expected a value", error);
      return nullptr;
    }
    advance();
    // Allow unquoted values such as file:line, e.g. `site = render.cpp:10`.
    while (value.kind == ParsedValue::Kind::String &&
           peek().kind == TokKind::Colon) {
      advance();
      const Token& part = peek();
      if (part.kind != TokKind::Word && part.kind != TokKind::Number) {
        fail(part.offset, "expected a value after ':'", error);
        return nullptr;
      }
      value.text += ":";
      value.text += part.text;
      advance();
      if (value.text.size() > kMaxLiteral) {
        fail(part.offset, "value too long", error);
        return nullptr;
      }
    }
    auto node = NodeBuilder::make(NodeKind::Compare);
    node->field = token.text;
    node->op = op;
    node->value = value;
    node->has_value = true;
    return node;
  }

  std::vector<Token> tokens_;
  std::string_view source_;
  size_t position_ = 0;
  int depth_ = 0;
};

std::optional<double> numeric_value(const Trace& trace, const Record& record,
                                    std::string_view field) {
  if (field == "ts" || field == "t") {
    return static_cast<double>(record.ts);
  }
  if (field == "thread" || field == "tid") {
    return static_cast<double>(record.thread);
  }
  if (field == "span") return static_cast<double>(record.span);
  if (field == "type") return std::nullopt;
  const Value* value = trace.field(record, field);
  if (value == nullptr) return std::nullopt;
  if (!value->is_numeric()) return std::nullopt;
  return value->as_double();
}

std::optional<std::string> string_value(const Trace& trace,
                                        const Record& record,
                                        std::string_view field) {
  if (field == "type") return std::string(to_string(record.type));
  const Value* value = trace.field(record, field);
  if (value == nullptr) return std::nullopt;
  switch (value->type) {
    case ValueType::String:
      return std::string(trace.text(value->s));
    case ValueType::Int:
      return std::to_string(value->i);
    case ValueType::Float:
      return std::to_string(value->f);
    case ValueType::Bool:
      return value->i != 0 ? "true" : "false";
  }
  return std::nullopt;
}

bool compare_op(TokKind op, int cmp) {
  switch (op) {
    case TokKind::Eq:
    case TokKind::Colon: return cmp == 0;
    case TokKind::Ne: return cmp != 0;
    case TokKind::Lt: return cmp < 0;
    case TokKind::Le: return cmp <= 0;
    case TokKind::Gt: return cmp > 0;
    case TokKind::Ge: return cmp >= 0;
    default: return false;
  }
}

bool matches_node(const Query::Node& node, const Trace& trace,
                  const Record& record) {
  switch (node.kind) {
    case NodeKind::And:
      if (node.children.size() != 2) return false;
      return matches_node(*node.children[0], trace, record) &&
             matches_node(*node.children[1], trace, record);
    case NodeKind::Or:
      if (node.children.size() != 2) return false;
      return matches_node(*node.children[0], trace, record) ||
             matches_node(*node.children[1], trace, record);
    case NodeKind::Not:
      if (node.children.size() != 1) return false;
      return !matches_node(*node.children[0], trace, record);
    case NodeKind::Text: {
      if (icontains(to_string(record.type), node.text)) return true;
      const auto fields = record.field_span(trace.fields);
      for (const auto& field : fields) {
        if (field.value.type != ValueType::String) continue;
        if (icontains(trace.text(field.value.s), node.text)) return true;
      }
      return false;
    }
    case NodeKind::Compare: {
      if (!node.has_value) return false;
      if (node.field == "type") {
        const auto text = to_string(record.type);
        if (node.value.kind == ParsedValue::Kind::String ||
            node.value.kind == ParsedValue::Kind::Bool) {
          if (node.op == TokKind::Contains) {
            return icontains(text, node.value.text);
          }
          if (node.op == TokKind::NotContains) {
            return !icontains(text, node.value.text);
          }
          const int cmp = iequals(text, node.value.text) ? 0
                          : (text < node.value.text ? -1 : 1);
          return compare_op(node.op, cmp);
        }
        return false;
      }
      if (node.value.kind == ParsedValue::Kind::Number) {
        const auto number = numeric_value(trace, record, node.field);
        if (!number.has_value()) return false;
        const double delta = *number - node.value.number;
        const int cmp = delta == 0 ? 0 : (delta < 0 ? -1 : 1);
        return compare_op(node.op, cmp);
      }
      const std::string needle = node.value.text;
      if (node.op == TokKind::Contains ||
          node.op == TokKind::NotContains) {
        const auto text = string_value(trace, record, node.field);
        if (!text.has_value()) return false;
        const bool found = icontains(*text, needle);
        return node.op == TokKind::Contains ? found : !found;
      }
      if (node.value.kind == ParsedValue::Kind::Bool) {
        const auto number = numeric_value(trace, record, node.field);
        if (number.has_value()) {
          const bool actual = *number != 0.0;
          const int cmp = actual == node.value.boolean
                              ? 0
                              : (actual ? 1 : -1);
          return compare_op(node.op, cmp);
        }
      }
      const auto text = string_value(trace, record, node.field);
      if (!text.has_value()) return false;
      const int cmp = iequals(*text, needle) ? 0 : (*text < needle ? -1 : 1);
      return compare_op(node.op, cmp);
    }
  }
  return false;
}

std::string render_node(const Query::Node& node);

std::string render_value(const Query::Node& node) {
  if (!node.has_value) return "?";
  switch (node.value.kind) {
    case ParsedValue::Kind::Number: {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%.10g", node.value.number);
      return buffer;
    }
    case ParsedValue::Kind::Bool:
      return node.value.boolean ? "true" : "false";
    case ParsedValue::Kind::String:
      return "\"" + node.value.text + "\"";
  }
  return "?";
}

std::string_view op_text(TokKind op) {
  switch (op) {
    case TokKind::Eq: return "=";
    case TokKind::Ne: return "!=";
    case TokKind::Lt: return "<";
    case TokKind::Le: return "<=";
    case TokKind::Gt: return ">";
    case TokKind::Ge: return ">=";
    case TokKind::Contains: return "~";
    case TokKind::NotContains: return "!~";
    case TokKind::Colon: return "=";
    default: return "?";
  }
}

std::string render_node(const Query::Node& node) {
  switch (node.kind) {
    case NodeKind::And:
      return "(" + render_node(*node.children[0]) + " AND " +
             render_node(*node.children[1]) + ")";
    case NodeKind::Or:
      return "(" + render_node(*node.children[0]) + " OR " +
             render_node(*node.children[1]) + ")";
    case NodeKind::Not:
      return "NOT " + render_node(*node.children[0]);
    case NodeKind::Text:
      return std::string(node.text);
    case NodeKind::Compare:
      return node.field + std::string(op_text(node.op)) + render_value(node);
  }
  return "?";
}

}  // namespace

Query::Query() = default;

Query Query::parse(std::string_view source, QueryError* error) {
  Query query;
  if (source.size() > kMaxQueryLength) {
    if (error) {
      error->offset = kMaxQueryLength;
      error->message = "query too long (limit 4096 bytes)";
    }
    return query;
  }
  query.source_ = std::string(source);
  if (source.empty()) return query;
  try {
    QueryError local;
    std::vector<Token> tokens;
    Lexer lexer(source);
    if (!lexer.lex(tokens, local)) {
      if (error) *error = local;
      return query;
    }
    Parser parser(tokens, source);
    auto root = parser.parse(local);
    if (!root) {
      if (error) *error = local;
      return query;
    }
    query.root_ = std::move(root);
    query.source_ = std::string(source);
    return query;
  } catch (const std::exception& e) {
    if (error) {
      error->offset = 0;
      error->message = std::string("query parser failure: ") + e.what();
    }
    return Query();
  }
}

bool Query::matches(const Trace& trace, const Record& record) const {
  if (!root_) return false;
  return matches_node(*root_, trace, record);
}

std::string Query::describe() const {
  if (!root_) return {};
  return render_node(*root_);
}

size_t Query::node_count() const { return root_ ? root_->count : 0; }

std::vector<uint32_t> run_query(const Trace& trace, const Query& query,
                                uint64_t begin_ts, uint64_t end_ts,
                                size_t limit, bool newest_first) {
  std::vector<uint32_t> out;
  if (!query.valid() || limit == 0) return out;
  limit = std::min<size_t>(limit, 10000);
  const auto [first, last] = trace.record_range(begin_ts, end_ts);
  if (first >= last) return out;
  if (newest_first) {
    for (size_t i = last; i > first; --i) {
      const size_t index = i - 1;
      if (query.matches(trace, trace.records[index])) {
        out.push_back(static_cast<uint32_t>(index));
        if (out.size() >= limit) break;
      }
    }
  } else {
    for (size_t i = first; i < last; ++i) {
      if (query.matches(trace, trace.records[i])) {
        out.push_back(static_cast<uint32_t>(i));
        if (out.size() >= limit) break;
      }
    }
  }
  return out;
}

}  // namespace gbx
