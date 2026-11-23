// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "glassbox/attributes.hpp"
#include "glassbox/status.hpp"

namespace gbx {

/// Hard limits for an interned string table. Growth past these limits fails
/// instead of exhausting memory.
struct StringTableLimits {
  size_t max_strings = 262144;
  size_t max_bytes = 32u << 20;
  size_t max_string_length = 64u << 10;
};

/// Append-only string interning used by trace records. Thread-compatibility:
/// not internally synchronized; each Trace owns one and it is only mutated by
/// the constructing thread before the trace is published.
class StringTable {
 public:
  explicit StringTable(StringTableLimits limits = {});

  /// Interns `text` and returns its id, or `std::nullopt` when a limit would
  /// be exceeded. Empty strings map to id 0 whose entry is "".
  std::optional<uint32_t> try_intern(std::string_view text);

  /// Interns without limit reporting. Prefer try_intern on untrusted input.
  uint32_t intern(std::string_view text) {
    return try_intern(text).value_or(kInvalidId);
  }

  std::optional<uint32_t> find(std::string_view text) const;

  /// Returns the string for `id`, or an empty view for out-of-range ids.
  std::string_view get(uint32_t id) const {
    return id < entries_.size() ? std::string_view(entries_[id]) : kEmpty;
  }

  size_t size() const { return entries_.size(); }
  size_t bytes() const { return bytes_; }
  bool full() const {
    return entries_.size() >= limits_.max_strings || bytes_ >= limits_.max_bytes;
  }
  const std::vector<std::string>& entries() const { return entries_; }
  const StringTableLimits& limits() const { return limits_; }

  static constexpr uint32_t kInvalidId = 0xffffffffu;

 private:
  static constexpr std::string_view kEmpty = "";
  StringTableLimits limits_;
  std::vector<std::string> entries_;
  std::map<std::string, uint32_t, std::less<>> index_;
  size_t bytes_ = 0;
};

/// Metadata describing the recorded process and session.
struct TraceMeta {
  std::string app_name;
  std::string app_version;
  std::string exe_path;
  std::string arch;
  std::string os;
  uint64_t wall_start_ns = 0;  // Unix epoch nanoseconds when recording started
  uint32_t pid = 0;
  uint64_t recorded_ns = 0;    // duration, filled by the writer/reader
};

/// One encoded chunk. The reader keeps offsets so tools can seek to a time
/// window without decoding the whole file again.
struct ChunkInfo {
  uint64_t file_offset = 0;
  uint64_t first_ts = 0;
  uint64_t last_ts = 0;
  uint32_t record_count = 0;
};

/// Bounds applied while loading untrusted trace files.
struct TraceLimits {
  size_t max_file_bytes = 2ull << 30;
  size_t max_records = 8'000'000;
  size_t max_fields = 32'000'000;
  size_t max_string_bytes = 128u << 20;
  size_t max_strings = 524288;
  size_t max_string_length = 64u << 10;
  /// Upper bound for the decoded in-memory model. Crossing it fails with
  /// LimitExceeded instead of risking the process.
  size_t max_memory_bytes = 1u << 30;
  uint32_t max_chunks = 65536;
  uint32_t max_threads = 65536;
  uint16_t max_record_fields = kMaxRecordFields;
};

/// Non-fatal conditions encountered while loading a trace.
struct TraceDiagnostics {
  bool truncated = false;   // the file ends mid-chunk
  bool recovered = false;   // truncated tail was dropped and the rest loaded
  bool checksum_mismatch = false;
  bool streamed = false;
  std::string warning;
  size_t bytes_read = 0;
};

/// An immutable-after-finalize in-memory trace: a flat, cache-friendly record
/// array plus one shared field array and string table. Ownership: records
/// refer to fields and strings by index, so the three arrays must stay
/// together.
class Trace {
 public:
  TraceMeta meta;
  std::vector<Record> records;
  std::vector<Field> fields;
  StringTable strings;
  std::vector<ChunkInfo> chunks;
  TraceDiagnostics diagnostics;

  explicit Trace(TraceLimits limits = {});

  /// Appends a record and its fields. Fails with LimitExceeded rather than
  /// growing without bound.
  Status add_record(Record record, std::span<const Field> record_fields);

  /// Sorts records by timestamp (stable, preserving emission order for equal
  /// timestamps), derives thread names and duration. Idempotent.
  void finalize();

  bool finalized() const { return finalized_; }
  bool empty() const { return records.empty(); }
  size_t size() const { return records.size(); }
  uint64_t end_ts() const { return end_ts_; }
  uint64_t duration() const { return end_ts_; }

  const Value* field(const Record& record, std::string_view key) const;
  const Value* field(const Record& record, uint32_t key_id) const;
  int64_t int_field(const Record& record, std::string_view key,
                    int64_t fallback = 0) const;
  double double_field(const Record& record, std::string_view key,
                      double fallback = 0.0) const;
  std::string_view str_field(const Record& record,
                             std::string_view key) const;
  std::string_view text(uint32_t string_id) const {
    return strings.get(string_id);
  }

  /// Thread name for a dense slot; empty when unknown.
  std::string_view thread_name(uint32_t thread) const;
  size_t thread_count() const { return thread_names_.size(); }

  /// Returns the record index range covering [begin_ts, end_ts] using the
  /// sorted record array. Result is a half-open [first, last) pair.
  std::pair<size_t, size_t> record_range(uint64_t begin_ts,
                                         uint64_t end_ts) const;

  const TraceLimits& limits() const { return limits_; }
  size_t memory_usage() const;
  size_t estimated_memory() const;

 private:
  TraceLimits limits_;
  std::vector<std::string> thread_names_;
  uint64_t end_ts_ = 0;
  bool finalized_ = false;
};

/// Iterates the key/value pairs stored under a snapshot record, resolving
/// string values through the trace.
class SnapshotView {
 public:
  SnapshotView(const Trace& trace, const Record& record);

  size_t size() const { return record_.field_count; }
  std::string_view key_at(size_t index) const;
  const Value& value_at(size_t index) const;
  std::optional<std::string_view> string_at(size_t index) const;

 private:
  const Trace& trace_;
  const Record& record_;
  std::span<const Field> fields_;
};

}  // namespace gbx
