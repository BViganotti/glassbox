// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/trace.hpp"

#include <algorithm>
#include <utility>

namespace gbx {

StringTable::StringTable(StringTableLimits limits) : limits_(limits) {
  try_intern("");
}

std::optional<uint32_t> StringTable::try_intern(std::string_view text) {
  if (text.size() > limits_.max_string_length) return std::nullopt;
  if (auto it = index_.find(text); it != index_.end()) {
    return it->second;
  }
  if (entries_.size() >= limits_.max_strings ||
      bytes_ + text.size() > limits_.max_bytes) {
    return std::nullopt;
  }
  const auto id = static_cast<uint32_t>(entries_.size());
  // Copy into the owning map key first so the string_view stays valid even if
  // the vector reallocates below.
  std::string owned(text);
  entries_.push_back(std::move(owned));
  index_.emplace(entries_.back(), id);
  bytes_ += text.size();
  return id;
}

std::optional<uint32_t> StringTable::find(std::string_view text) const {
  auto it = index_.find(text);
  if (it == index_.end()) return std::nullopt;
  return it->second;
}

Trace::Trace(TraceLimits limits)
    : strings(StringTableLimits{limits.max_strings, limits.max_string_bytes,
                                limits.max_string_length}),
      limits_(limits) {}

Status Trace::add_record(Record record, std::span<const Field> record_fields) {
  if (record_fields.size() > limits_.max_record_fields) {
    return Status::error(ErrorCode::LimitExceeded, "record has too many fields");
  }
  if (records.size() >= limits_.max_records) {
    return Status::error(ErrorCode::LimitExceeded, "record limit reached");
  }
  if (fields.size() + record_fields.size() > limits_.max_fields) {
    return Status::error(ErrorCode::LimitExceeded, "field limit reached");
  }
  if (record.thread >= limits_.max_threads) {
    return Status::error(ErrorCode::LimitExceeded, "thread slot out of range");
  }
  if (!record_fields.empty() && record_fields.size() > kMaxRecordFields) {
    return Status::error(ErrorCode::LimitExceeded, "record has too many fields");
  }
  if (record_fields.size() > 0xffff) {
    return Status::error(ErrorCode::LimitExceeded, "record field count overflow");
  }
  const size_t projected = estimated_memory() + sizeof(Record) +
                           record_fields.size() * sizeof(Field);
  if (projected > limits_.max_memory_bytes) {
    return Status::error(ErrorCode::LimitExceeded,
                         "decoded trace memory budget exceeded");
  }
  record.first_field = static_cast<uint32_t>(fields.size());
  record.field_count = static_cast<uint16_t>(record_fields.size());
  fields.insert(fields.end(), record_fields.begin(), record_fields.end());
  records.push_back(record);
  finalized_ = false;
  return Status::success();
}

void Trace::finalize() {
  if (finalized_) return;
  std::stable_sort(records.begin(), records.end(),
                   [](const Record& a, const Record& b) { return a.ts < b.ts; });
  end_ts_ = records.empty() ? 0 : records.back().ts;
  thread_names_.clear();
  uint32_t max_thread = 0;
  for (const auto& record : records) {
    max_thread = std::max(max_thread, record.thread);
  }
  if (max_thread > 0 || !records.empty()) {
    thread_names_.resize(static_cast<size_t>(max_thread) + 1);
  }
  for (const auto& record : records) {
    if (record.type != RecordType::ThreadName) continue;
    const auto name = str_field(record, keys::kName);
    if (!name.empty()) thread_names_[record.thread] = std::string(name);
  }
  finalized_ = true;
}

const Value* Trace::field(const Record& record, std::string_view key) const {
  const auto span = record.field_span(fields);
  for (const auto& f : span) {
    if (strings.get(f.key) == key) return &f.value;
  }
  return nullptr;
}

const Value* Trace::field(const Record& record, uint32_t key_id) const {
  const auto span = record.field_span(fields);
  for (const auto& f : span) {
    if (f.key == key_id) return &f.value;
  }
  return nullptr;
}

int64_t Trace::int_field(const Record& record, std::string_view key,
                         int64_t fallback) const {
  const Value* v = field(record, key);
  return v ? v->as_int() : fallback;
}

double Trace::double_field(const Record& record, std::string_view key,
                           double fallback) const {
  const Value* v = field(record, key);
  return v ? v->as_double() : fallback;
}

std::string_view Trace::str_field(const Record& record,
                                  std::string_view key) const {
  const Value* v = field(record, key);
  if (!v || v->type != ValueType::String) return {};
  return strings.get(v->s);
}

std::string_view Trace::thread_name(uint32_t thread) const {
  if (thread < thread_names_.size()) return thread_names_[thread];
  return {};
}

std::pair<size_t, size_t> Trace::record_range(uint64_t begin_ts,
                                              uint64_t end_ts) const {
  if (!finalized_ || records.empty() || end_ts < begin_ts) return {0, 0};
  const auto first =
      std::lower_bound(records.begin(), records.end(), begin_ts,
                       [](const Record& r, uint64_t ts) { return r.ts < ts; });
  const auto last =
      std::upper_bound(records.begin(), records.end(), end_ts,
                       [](uint64_t ts, const Record& r) { return ts < r.ts; });
  if (first >= last) return {0, 0};
  return {static_cast<size_t>(first - records.begin()),
          static_cast<size_t>(last - records.begin())};
}

size_t Trace::estimated_memory() const {
  return sizeof(Trace) + records.capacity() * sizeof(Record) +
         fields.capacity() * sizeof(Field) + strings.bytes() +
         chunks.capacity() * sizeof(ChunkInfo);
}

size_t Trace::memory_usage() const {
  size_t total = sizeof(*this);
  total += records.capacity() * sizeof(Record);
  total += fields.capacity() * sizeof(Field);
  total += strings.bytes();
  total += chunks.capacity() * sizeof(ChunkInfo);
  total += diagnostics.warning.capacity();
  for (const auto& name : thread_names_) total += name.capacity();
  return total;
}

SnapshotView::SnapshotView(const Trace& trace, const Record& record)
    : trace_(trace), record_(record), fields_(record.field_span(trace.fields)) {}

std::string_view SnapshotView::key_at(size_t index) const {
  if (index >= fields_.size()) return {};
  return trace_.strings.get(fields_[index].key);
}

const Value& SnapshotView::value_at(size_t index) const {
  static const Value empty;
  if (index >= fields_.size()) return empty;
  return fields_[index].value;
}

std::optional<std::string_view> SnapshotView::string_at(size_t index) const {
  if (index >= fields_.size()) return std::nullopt;
  const Value& v = fields_[index].value;
  if (v.type != ValueType::String) return std::nullopt;
  return trace_.strings.get(v.s);
}

}  // namespace gbx
