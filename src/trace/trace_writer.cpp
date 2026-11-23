// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "trace/trace_writer.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <limits>
#include <utility>

#include "trace/crc32.hpp"
#include "trace/format.hpp"
#include "trace/varint.hpp"

namespace gbx::trace {
namespace {

Status write_length_string(FileWriter& file, std::string_view text) {
  if (text.size() > 0xffffffffu) {
    return Status::error(ErrorCode::LimitExceeded, "meta string too large");
  }
  auto status = file.write_u32(static_cast<uint32_t>(text.size()));
  if (!status.ok()) return status;
  return file.write(text.data(), text.size());
}

}  // namespace

TraceWriter::~TraceWriter() {
  if (open_) abort();
}

Status TraceWriter::open(const std::string& path, const TraceMeta& meta,
                         SaveOptions options) {
  if (open_) return Status::error(ErrorCode::Busy, "writer already open");
  options_ = options;
  if (options_.chunk_bytes == 0) options_.chunk_bytes = 1u << 20;
  if (options_.chunk_records == 0) options_.chunk_records = 50000;
  if (options_.chunk_strings == 0) options_.chunk_strings = 4096;

  auto status = file_.open(path);
  if (!status.ok()) return status;
  status = file_.write(format::kMagic.data(), format::kMagic.size());
  if (status.ok()) status = file_.write_u32(format::kVersion);
  if (status.ok()) status = file_.write_u32(0);  // flags
  if (status.ok()) status = file_.write_u64(meta.wall_start_ns);
  if (status.ok()) status = file_.write_u32(meta.pid);
  constexpr uint32_t kMetaFields = 5;
  if (status.ok()) status = file_.write_u32(kMetaFields);
  if (status.ok()) status = write_length_string(file_, meta.app_name);
  if (status.ok()) status = write_length_string(file_, meta.app_version);
  if (status.ok()) status = write_length_string(file_, meta.exe_path);
  if (status.ok()) status = write_length_string(file_, meta.arch);
  if (status.ok()) status = write_length_string(file_, meta.os);
  if (!status.ok()) {
    file_.close();
    return wrap(status, "writing trace header");
  }
  open_ = true;
  error_ = Status::success();
  return Status::success();
}

Status TraceWriter::prepare_for_record(std::span<const Field> fields,
                                       const StringTable& strings) {
  if (chunk_records_ == 0) return Status::success();
  size_t extra_bytes = 0;
  size_t extra_count = 0;
  uint32_t seen[2 * kMaxRecordFields + 2];
  size_t seen_count = 0;
  auto need = [&](uint32_t global_id) -> Status {
    if (local_of_global_.find(global_id) != local_of_global_.end()) {
      return Status::success();
    }
    for (size_t i = 0; i < seen_count; ++i) {
      if (seen[i] == global_id) return Status::success();
    }
    if (global_id >= strings.size()) {
      return Status::error(ErrorCode::InvalidArgument,
                           "record references unknown string id");
    }
    seen[seen_count++] = global_id;
    extra_bytes += strings.get(global_id).size();
    ++extra_count;
    return Status::success();
  };
  for (const auto& field : fields) {
    auto status = need(field.key);
    if (!status.ok()) return status;
    if (field.value.type == ValueType::String) {
      status = need(field.value.s);
      if (!status.ok()) return status;
    }
  }
  if (chunk_body_.size() + chunk_string_bytes_ + extra_bytes >=
          options_.chunk_bytes ||
      chunk_string_values_.size() + extra_count > options_.chunk_strings ||
      chunk_records_ >= options_.chunk_records) {
    return flush_chunk();
  }
  return Status::success();
}

uint32_t TraceWriter::local_string(uint32_t global_id,
                                   const StringTable& strings,
                                   Status& status) {
  auto it = local_of_global_.find(global_id);
  if (it != local_of_global_.end()) return it->second;
  if (global_id >= strings.size()) {
    status = Status::error(ErrorCode::InvalidArgument,
                           "record references unknown string id");
    return 0;
  }
  const std::string_view text = strings.get(global_id);
  if (text.size() > strings.limits().max_string_length ||
      chunk_string_values_.size() >= format::kMaxChunkStrings ||
      chunk_string_bytes_ + text.size() > format::kMaxChunkPayload / 2) {
    status = Status::error(ErrorCode::LimitExceeded, "string budget exceeded");
    return 0;
  }
  const auto local = static_cast<uint32_t>(chunk_string_values_.size());
  chunk_string_values_.emplace_back(text);
  chunk_string_bytes_ += text.size();
  local_of_global_.emplace(global_id, local);
  return local;
}

Status TraceWriter::append(const Record& record,
                           std::span<const Field> fields,
                           const StringTable& strings) {
  if (!open_) {
    return error_.ok() ? Status::error(ErrorCode::Busy, "writer closed")
                       : error_;
  }
  if (!error_.ok()) return error_;
  if (fields.size() > kMaxRecordFields) {
    return fail(Status::error(ErrorCode::InvalidArgument,
                              "record has too many fields"));
  }
  if (record.ts > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return fail(Status::error(ErrorCode::LimitExceeded,
                              "timestamp exceeds encoder range"));
  }
  if (record.type > RecordType::Custom) {
    return fail(
        Status::error(ErrorCode::InvalidArgument, "unknown record type"));
  }
  auto prep = prepare_for_record(fields, strings);
  if (!prep.ok()) return fail(prep);

  record_buf_.clear();
  ByteWriter writer(record_buf_);
  if (!chunk_has_records_) {
    writer.varint(record.ts);
  } else if (record.ts >= prev_ts_) {
    const uint64_t delta = record.ts - prev_ts_;
    if (delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return fail(Status::error(ErrorCode::LimitExceeded, "delta overflow"));
    }
    writer.svarint(static_cast<int64_t>(delta));
  } else {
    const uint64_t delta = prev_ts_ - record.ts;
    const uint64_t limit =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (delta > limit) {
      return fail(Status::error(ErrorCode::LimitExceeded, "delta overflow"));
    }
    writer.svarint(-static_cast<int64_t>(delta));
  }
  writer.u8(static_cast<uint8_t>(record.type));
  writer.varint(record.thread);
  writer.varint(record.span);
  writer.varint(fields.size());

  Status status = Status::success();
  for (const auto& field : fields) {
    const uint32_t key_local = local_string(field.key, strings, status);
    if (!status.ok()) return fail(status);
    writer.varint(key_local);
    writer.u8(static_cast<uint8_t>(field.value.type));
    switch (field.value.type) {
      case ValueType::Int:
        writer.svarint(field.value.i);
        break;
      case ValueType::Float:
        writer.f64(field.value.f);
        break;
      case ValueType::String: {
        const uint32_t value_local =
            local_string(field.value.s, strings, status);
        if (!status.ok()) return fail(status);
        writer.varint(value_local);
        break;
      }
      case ValueType::Bool:
        writer.u8(field.value.i != 0 ? 1 : 0);
        break;
      default:
        return fail(Status::error(ErrorCode::InvalidArgument,
                                  "unknown value type"));
    }
  }

  if (chunk_body_.size() + record_buf_.size() + chunk_string_bytes_ >
      format::kMaxChunkPayload) {
    return fail(Status::error(ErrorCode::LimitExceeded,
                              "chunk exceeds maximum payload"));
  }
  chunk_body_.insert(chunk_body_.end(), record_buf_.begin(), record_buf_.end());
  if (!chunk_has_records_) chunk_has_records_ = true;
  prev_ts_ = record.ts;
  max_ts_ = std::max(max_ts_, record.ts);
  ++chunk_records_;
  ++record_count_;
  return Status::success();
}

Status TraceWriter::flush_chunk() {
  if (!open_) return error_;
  if (!error_.ok()) return error_;
  if (!chunk_has_records_) return Status::success();

  std::vector<uint8_t> payload;
  payload.reserve(chunk_body_.size() + chunk_string_bytes_ + 32);
  ByteWriter writer(payload);
  writer.varint(chunk_string_values_.size());
  for (const auto& text : chunk_string_values_) {
    if (text.size() > 0xffffffffu) {
      return fail(Status::error(ErrorCode::LimitExceeded, "string too large"));
    }
    writer.varint(text.size());
    writer.raw(text.data(), text.size());
  }
  writer.varint(chunk_records_);
  payload.insert(payload.end(), chunk_body_.begin(), chunk_body_.end());
  if (payload.size() > format::kMaxChunkPayload) {
    return fail(Status::error(ErrorCode::LimitExceeded,
                              "chunk payload exceeds limit"));
  }

  const uint32_t crc = crc32(payload);
  auto status =
      file_.write(format::kChunkMagic.data(), format::kChunkMagic.size());
  if (status.ok()) {
    status = file_.write_u32(static_cast<uint32_t>(payload.size()));
  }
  if (status.ok()) status = file_.write_u32(crc);
  if (status.ok()) status = file_.write_u32(chunk_records_);
  if (status.ok()) status = file_.write(payload);
  if (!status.ok()) return fail(status);

  ++chunk_count_;
  chunk_body_.clear();
  chunk_string_values_.clear();
  local_of_global_.clear();
  chunk_string_bytes_ = 0;
  chunk_records_ = 0;
  chunk_has_records_ = false;
  return Status::success();
}

Status TraceWriter::finish() {
  if (!open_) {
    return finished_
               ? Status::success()
               : (error_.ok() ? Status::error(ErrorCode::Busy, "writer closed")
                              : error_);
  }
  if (!error_.ok()) return error_;
  auto status = flush_chunk();
  if (!status.ok()) return status;

  std::vector<uint8_t> footer_tail;
  footer_tail.reserve(24);
  auto append_u64 = [&footer_tail](uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      footer_tail.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
  };
  append_u64(chunk_count_);
  append_u64(record_count_);
  append_u64(max_ts_);
  const uint32_t crc = crc32(footer_tail);

  status = file_.write(format::kFooterMagic.data(), format::kFooterMagic.size());
  if (status.ok()) status = file_.write(footer_tail);
  if (status.ok()) status = file_.write_u32(crc);
  if (!status.ok()) return fail(status);
  if (options_.sync_on_close) {
    status = file_.sync();
    if (!status.ok()) return fail(status);
  }
  status = file_.close();
  open_ = false;
  if (!status.ok()) return fail(status);
  finished_ = true;
  return Status::success();
}

Status TraceWriter::abort() {
  if (open_) {
    file_.close();
    open_ = false;
  }
  finished_ = true;
  return Status::success();
}

Status TraceWriter::fail(Status status) {
  if (error_.ok()) error_ = std::move(status);
  if (open_) {
    file_.close();
    open_ = false;
  }
  return error_;
}

}  // namespace gbx::trace

namespace gbx {
namespace {

uint64_t wall_clock_ns() {
  struct timespec ts {};
  if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

TraceMeta current_meta(std::string_view app_name, std::string_view app_version,
                       std::string_view exe_path) {
  TraceMeta meta;
  meta.app_name = std::string(app_name);
  meta.app_version = std::string(app_version);
  meta.exe_path = std::string(exe_path);
  meta.wall_start_ns = wall_clock_ns();
  meta.pid = static_cast<uint32_t>(::getpid());
  struct utsname info {};
  if (::uname(&info) == 0) {
    meta.arch = info.machine;
    meta.os = std::string(info.sysname) + " " + info.release;
  }
  return meta;
}

Status save_trace(const std::string& path, const Trace& trace,
                  const SaveOptions& options) {
  gbx::trace::TraceWriter writer;
  auto status = writer.open(path, trace.meta, options);
  if (!status.ok()) return wrap(status, "opening trace for write");
  for (const auto& record : trace.records) {
    status = writer.append(record, record.field_span(trace.fields),
                           trace.strings);
    if (!status.ok()) {
      writer.abort();
      return wrap(status, "writing trace record");
    }
  }
  return writer.finish();
}

}  // namespace gbx
