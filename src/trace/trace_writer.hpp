// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "glassbox/trace_io.hpp"
#include "trace/file_io.hpp"

namespace gbx::trace {

/// Streaming trace encoder. Not thread-safe: exactly one thread may append.
/// The StringTable passed to append must outlive the call and contain every
/// string id referenced by the record.
class TraceWriter {
 public:
  TraceWriter() = default;
  ~TraceWriter();
  TraceWriter(const TraceWriter&) = delete;
  TraceWriter& operator=(const TraceWriter&) = delete;

  Status open(const std::string& path, const TraceMeta& meta,
              SaveOptions options = {});
  Status append(const Record& record, std::span<const Field> fields,
                const StringTable& strings);
  Status finish();
  Status abort();

  bool is_open() const { return open_; }
  uint64_t offset() const { return file_.offset(); }
  uint32_t chunk_count() const { return chunk_count_; }
  uint64_t record_count() const { return record_count_; }
  uint64_t max_ts() const { return max_ts_; }

 private:
  Status flush_chunk();
  Status fail(Status status);
  Status prepare_for_record(std::span<const Field> fields,
                            const StringTable& strings);
  uint32_t local_string(uint32_t global_id, const StringTable& strings,
                        Status& status);

  FileWriter file_;
  SaveOptions options_;
  Status error_;
  bool open_ = false;
  bool finished_ = false;

  std::vector<uint8_t> chunk_body_;
  std::vector<uint8_t> record_buf_;
  std::vector<std::string> chunk_string_values_;
  std::unordered_map<uint32_t, uint32_t> local_of_global_;
  size_t chunk_string_bytes_ = 0;

  uint32_t chunk_records_ = 0;
  uint64_t prev_ts_ = 0;
  bool chunk_has_records_ = false;
  uint32_t chunk_count_ = 0;
  uint64_t record_count_ = 0;
  uint64_t max_ts_ = 0;
};

}  // namespace gbx::trace
