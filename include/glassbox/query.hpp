// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "glassbox/trace.hpp"

namespace gbx {

struct QueryError {
  size_t offset = 0;
  std::string message;
};

/// A parsed record predicate.
///
/// Grammar (case-insensitive keywords):
///   expr      := or
///   or        := and (("OR" | "||") and)*
///   and       := unary (("AND" | "&&") unary)*
///   unary     := ("NOT" | "!") unary | primary
///   primary   := "(" expr ")" | comparison | text
///   comparison:= field (":" | "=" | "==" | "!=" | "<" | "<=" | ">" | ">="
///                       | "~" | "!~") value
///   value     := number | duration ("250us", "1.5ms", "2s") | string | bool
///
/// Fields: type, ts/t, thread/tid, span, level, size, ptr, owner, and any
/// attribute key stored in the record (name, msg, site, class, ...). `~` is a
/// case-insensitive substring match. A bare word performs a case-insensitive
/// text search over the record's string fields and type name.
///
/// Limits: source length 4096, 256 tokens, nesting depth 32, literal length
/// 512. Invalid queries never throw; parse() returns an invalid Query and
/// fills QueryError.
class Query {
 public:
  /// Opaque parsed expression node.
  struct Node;

  Query();

  static Query parse(std::string_view source, QueryError* error = nullptr);

  bool valid() const { return root_ != nullptr; }
  const std::string& source() const { return source_; }

  /// True when the record matches. Invalid queries never match.
  bool matches(const Trace& trace, const Record& record) const;

  /// Normalized rendering of the parsed expression (empty when invalid).
  std::string describe() const;

  size_t node_count() const;

 private:
  std::shared_ptr<const Node> root_;
  std::string source_;
};

/// Collects matching record indices in [begin_ts, end_ts]. Stops after
/// `limit` matches (clamped to 10000). Indices refer to trace.records.
std::vector<uint32_t> run_query(const Trace& trace, const Query& query,
                                uint64_t begin_ts, uint64_t end_ts,
                                size_t limit = 500,
                                bool newest_first = false);

}  // namespace gbx
