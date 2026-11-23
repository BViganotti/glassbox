// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/trace_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "trace/crc32.hpp"
#include "trace/file_io.hpp"
#include "trace/format.hpp"
#include "trace/varint.hpp"

namespace gbx {
namespace {

using trace::ByteReader;
using namespace trace::format;

Status corrupt(std::string_view what) {
  return Status::error(ErrorCode::Corrupt, std::string(what));
}

Status decode_meta_string(ByteReader& reader, const TraceLimits& limits,
                          TraceMeta& meta, size_t index) {
  uint32_t length = 0;
  if (!reader.u32(length)) return corrupt("truncated metadata");
  if (length > limits.max_string_length) {
    return Status::error(ErrorCode::LimitExceeded, "metadata string too long");
  }
  std::string text(length, '\0');
  if (!reader.raw(text.data(), text.size())) {
    return corrupt("truncated metadata string");
  }
  switch (index) {
    case 0: meta.app_name = std::move(text); break;
    case 1: meta.app_version = std::move(text); break;
    case 2: meta.exe_path = std::move(text); break;
    case 3: meta.arch = std::move(text); break;
    case 4: meta.os = std::move(text); break;
    default: break;  // forwards-compatible extra metadata
  }
  return Status::success();
}

Status decode_chunk(ByteReader& reader, const TraceLimits& limits,
                    Trace& out, std::vector<uint32_t>& local_to_global,
                    uint32_t& record_count_out) {
  uint64_t string_count = 0;
  if (!reader.varint(string_count)) return corrupt("truncated chunk strings");
  if (string_count > kMaxChunkStrings || string_count > limits.max_strings) {
    return Status::error(ErrorCode::LimitExceeded, "chunk string count");
  }
  local_to_global.clear();
  local_to_global.reserve(static_cast<size_t>(string_count));
  for (uint64_t i = 0; i < string_count; ++i) {
    uint64_t length = 0;
    if (!reader.varint(length)) return corrupt("truncated chunk string");
    if (length > limits.max_string_length) {
      return Status::error(ErrorCode::LimitExceeded, "chunk string too long");
    }
    if (length > reader.remaining()) return corrupt("string length overrun");
    std::string text(static_cast<size_t>(length), '\0');
    if (length > 0 && !reader.raw(text.data(), text.size())) {
      return corrupt("truncated chunk string body");
    }
    auto id = out.strings.try_intern(text);
    if (!id.has_value()) {
      return Status::error(ErrorCode::LimitExceeded, "string table limit");
    }
    local_to_global.push_back(*id);
  }

  uint64_t record_count = 0;
  if (!reader.varint(record_count)) return corrupt("truncated record count");
  if (record_count > kMaxChunkRecords) {
    return Status::error(ErrorCode::LimitExceeded, "chunk record count");
  }
  if (out.records.size() + record_count > limits.max_records) {
    return Status::error(ErrorCode::LimitExceeded, "record limit reached");
  }
  record_count_out = static_cast<uint32_t>(record_count);

  uint64_t prev_ts = 0;
  bool has_ts = false;
  std::vector<Field> fields;
  for (uint64_t i = 0; i < record_count; ++i) {
    uint64_t raw_ts = 0;
    uint64_t ts = 0;
    if (!has_ts) {
      if (!reader.varint(raw_ts)) return corrupt("truncated timestamp");
      ts = raw_ts;
      if (ts > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return corrupt("timestamp out of range");
      }
    } else {
      int64_t delta = 0;
      if (!reader.svarint(delta)) return corrupt("truncated timestamp delta");
      if (delta >= 0) {
        const uint64_t d = static_cast<uint64_t>(delta);
        if (d > std::numeric_limits<uint64_t>::max() - prev_ts) {
          return corrupt("timestamp overflow");
        }
        ts = prev_ts + d;
      } else {
        const uint64_t d = static_cast<uint64_t>(-(delta + 1)) + 1;
        if (d > prev_ts) return corrupt("timestamp underflow");
        ts = prev_ts - d;
      }
      if (ts > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return corrupt("timestamp out of range");
      }
    }
    prev_ts = ts;
    has_ts = true;

    uint8_t type_byte = 0;
    if (!reader.u8(type_byte)) return corrupt("truncated record type");
    if (type_byte > static_cast<uint8_t>(RecordType::Custom)) {
      return corrupt("unknown record type");
    }
    uint64_t thread = 0;
    uint64_t span = 0;
    uint64_t field_count = 0;
    if (!reader.varint(thread) || !reader.varint(span) ||
        !reader.varint(field_count)) {
      return corrupt("truncated record header");
    }
    if (thread >= limits.max_threads) {
      return Status::error(ErrorCode::LimitExceeded, "thread slot out of range");
    }
    if (span > std::numeric_limits<uint32_t>::max()) {
      return corrupt("span id out of range");
    }
    if (field_count > limits.max_record_fields ||
        field_count > kMaxRecordFields) {
      return Status::error(ErrorCode::LimitExceeded, "too many record fields");
    }
    if (field_count > reader.remaining()) return corrupt("field count overrun");

    fields.clear();
    for (uint64_t f = 0; f < field_count; ++f) {
      uint64_t key_local = 0;
      uint8_t value_type = 0;
      if (!reader.varint(key_local) || !reader.u8(value_type)) {
        return corrupt("truncated field header");
      }
      if (key_local >= local_to_global.size()) {
        return corrupt("field key out of range");
      }
      Field field;
      field.key = local_to_global[static_cast<size_t>(key_local)];
      switch (static_cast<ValueType>(value_type)) {
        case ValueType::Int: {
          int64_t v = 0;
          if (!reader.svarint(v)) return corrupt("truncated int field");
          field.value = Value::integer(v);
          break;
        }
        case ValueType::Float: {
          double v = 0;
          if (!reader.f64(v)) return corrupt("truncated float field");
          if (!std::isfinite(v)) return corrupt("non-finite float field");
          field.value = Value::real(v);
          break;
        }
        case ValueType::String: {
          uint64_t local = 0;
          if (!reader.varint(local)) return corrupt("truncated string field");
          if (local >= local_to_global.size()) {
            return corrupt("string field out of range");
          }
          field.value = Value::str(local_to_global[static_cast<size_t>(local)]);
          break;
        }
        case ValueType::Bool: {
          uint8_t v = 0;
          if (!reader.u8(v)) return corrupt("truncated bool field");
          if (v > 1) return corrupt("invalid bool field");
          field.value = Value::boolean(v != 0);
          break;
        }
        default:
          return corrupt("unknown value type");
      }
      fields.push_back(field);
    }

    Record record;
    record.ts = ts;
    record.thread = static_cast<uint32_t>(thread);
    record.span = static_cast<uint32_t>(span);
    record.type = static_cast<RecordType>(type_byte);
    auto status = out.add_record(record, fields);
    if (!status.ok()) return status;
  }
  if (reader.remaining() != 0) return corrupt("trailing bytes in chunk");
  return Status::success();
}

Status load_impl(std::span<const uint8_t> bytes, Trace& out,
                 const LoadOptions& options, const std::string& source) {
  out = Trace(options.limits);
  if (bytes.size() < kFileHeaderFixed) {
    return Status::error(ErrorCode::Truncated, source + " is too short");
  }
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return corrupt(source + ": bad magic");
  }
  ByteReader reader(bytes);
  if (!reader.skip(kMagic.size())) return corrupt("truncated magic");
  uint32_t version = 0;
  uint32_t flags = 0;
  uint64_t wall_ns = 0;
  uint32_t pid = 0;
  uint32_t meta_count = 0;
  if (!reader.u32(version) || !reader.u32(flags) || !reader.u64(wall_ns) ||
      !reader.u32(pid) || !reader.u32(meta_count)) {
    return corrupt("truncated header");
  }
  (void)flags;
  if (version != kVersion) {
    return Status::error(ErrorCode::Unsupported,
                         "unsupported trace version " + std::to_string(version));
  }
  if (meta_count > kMaxMetaStrings) {
    return Status::error(ErrorCode::LimitExceeded, "metadata count");
  }
  out.meta.wall_start_ns = wall_ns;
  out.meta.pid = pid;
  for (uint32_t i = 0; i < meta_count; ++i) {
    auto status = decode_meta_string(reader, options.limits, out.meta, i);
    if (!status.ok()) return status;
  }

  const size_t header_end = reader.offset();
  bool saw_footer = false;
  uint64_t footer_chunks = 0;
  uint64_t footer_records = 0;
  uint64_t footer_duration = 0;
  std::vector<uint32_t> local_to_global;

  auto recovery = [&](std::string_view why) -> Status {
    out.diagnostics.truncated = true;
    if (!options.recover_truncated) {
      return Status::error(ErrorCode::Truncated, std::string(why));
    }
    out.diagnostics.recovered = true;
    out.diagnostics.warning = std::string(why);
    return Status::success();
  };

  while (reader.remaining() > 0) {
    const size_t chunk_offset = reader.offset();
    if (reader.remaining() < 8) {
      auto status = recovery(source + ": trailing partial record");
      if (!status.ok()) return status;
      break;
    }
    const auto magic = reader.peek(8);
    if (std::memcmp(magic.data(), kFooterMagic.data(), 8) == 0) {
      if (reader.remaining() < kFooterSize) {
        auto status = recovery(source + ": partial footer");
        if (!status.ok()) return status;
        break;
      }
      reader.skip(8);
      std::vector<uint8_t> tail(24);
      if (!reader.raw(tail.data(), tail.size())) return corrupt("footer body");
      uint32_t stored_crc = 0;
      if (!reader.u32(stored_crc)) return corrupt("footer crc");
      if (trace::crc32(tail) != stored_crc) {
        return corrupt("footer checksum mismatch");
      }
      ByteReader tail_reader(tail);
      tail_reader.u64(footer_chunks);
      tail_reader.u64(footer_records);
      tail_reader.u64(footer_duration);
      saw_footer = true;
      break;
    }
    if (std::memcmp(magic.data(), kChunkMagic.data(), 8) != 0) {
      auto status = recovery(source + ": unknown bytes after chunks");
      if (!status.ok()) return status;
      break;
    }
    if (reader.remaining() < kChunkHeaderSize) {
      auto status = recovery(source + ": partial chunk header");
      if (!status.ok()) return status;
      break;
    }
    reader.skip(8);
    uint32_t payload_len = 0;
    uint32_t stored_crc = 0;
    uint32_t chunk_records = 0;
    if (!reader.u32(payload_len) || !reader.u32(stored_crc) ||
        !reader.u32(chunk_records)) {
      return corrupt("truncated chunk header");
    }
    if (payload_len > kMaxChunkPayload) {
      return Status::error(ErrorCode::LimitExceeded, "chunk payload too large");
    }
    if (reader.remaining() < payload_len) {
      auto status = recovery(source + ": truncated chunk payload");
      if (!status.ok()) return status;
      break;
    }
    const auto payload = reader.peek(payload_len);
    if (options.verify_checksums && trace::crc32(payload) != stored_crc) {
      return corrupt("chunk checksum mismatch");
    }
    reader.skip(payload_len);
    if (out.chunks.size() >= options.limits.max_chunks) {
      return Status::error(ErrorCode::LimitExceeded, "chunk count limit");
    }
    ByteReader payload_reader(payload);
    uint32_t decoded_records = 0;
    const size_t first_record = out.records.size();
    auto status = decode_chunk(payload_reader, options.limits, out,
                               local_to_global, decoded_records);
    if (!status.ok()) return status;
    if (decoded_records != chunk_records) {
      return corrupt("chunk record count mismatch");
    }
    ChunkInfo info;
    info.file_offset = chunk_offset;
    info.record_count = decoded_records;
    if (first_record < out.records.size()) {
      info.first_ts = out.records[first_record].ts;
      info.last_ts = info.first_ts;
      for (size_t i = first_record; i < out.records.size(); ++i) {
        info.first_ts = std::min(info.first_ts, out.records[i].ts);
        info.last_ts = std::max(info.last_ts, out.records[i].ts);
      }
    }
    out.chunks.push_back(info);
  }

  if (reader.remaining() == 0 && !saw_footer) {
    auto status = recovery(source + ": missing footer");
    if (!status.ok()) return status;
  }
  if (reader.remaining() > 0 && saw_footer) {
    return corrupt("trailing bytes after footer");
  }
  if (saw_footer) {
    if (footer_chunks != out.chunks.size() ||
        footer_records != out.records.size()) {
      return corrupt("footer does not match decoded content");
    }
    if (!out.records.empty() && footer_duration < out.records.back().ts) {
      return corrupt("footer duration precedes the last record");
    }
  }
  out.meta.recorded_ns =
      saw_footer ? footer_duration
                 : (out.records.empty() ? 0 : out.records.back().ts);
  out.diagnostics.bytes_read = bytes.size();
  out.finalize();
  (void)header_end;
  return Status::success();
}

Status load_streamed(const std::string& path, Trace& out,
                     const LoadOptions& options) {
  out = Trace(options.limits);
  trace::FileReader file;
  auto status = file.open(path, options.limits.max_file_bytes);
  if (!status.ok()) return status;
  const auto recovery = [&](std::string_view why) -> Status {
    out.diagnostics.truncated = true;
    if (!options.recover_truncated) {
      return Status::error(ErrorCode::Truncated, std::string(why));
    }
    out.diagnostics.recovered = true;
    if (out.diagnostics.warning.empty()) {
      out.diagnostics.warning = std::string(why);
    }
    return Status::success();
  };
  if (file.size() < kFileHeaderFixed) {
    return Status::error(ErrorCode::Truncated, path + " is too short");
  }
  std::vector<uint8_t> header(kFileHeaderFixed);
  status = file.read_exact(header, header.size());
  if (!status.ok()) return status;
  if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
    return corrupt(path + ": bad magic");
  }
  ByteReader header_reader(header);
  header_reader.skip(kMagic.size());
  uint32_t version = 0;
  uint32_t flags = 0;
  uint64_t wall_ns = 0;
  uint32_t pid = 0;
  uint32_t meta_count = 0;
  if (!header_reader.u32(version) || !header_reader.u32(flags) ||
      !header_reader.u64(wall_ns) || !header_reader.u32(pid) ||
      !header_reader.u32(meta_count)) {
    return corrupt("truncated header");
  }
  (void)flags;
  if (version != kVersion) {
    return Status::error(ErrorCode::Unsupported,
                         "unsupported trace version " + std::to_string(version));
  }
  if (meta_count > kMaxMetaStrings) {
    return Status::error(ErrorCode::LimitExceeded, "metadata count");
  }
  out.meta.wall_start_ns = wall_ns;
  out.meta.pid = pid;
  for (uint32_t i = 0; i < meta_count; ++i) {
    if (file.remaining() < 4) {
      return Status::error(ErrorCode::Truncated, "truncated metadata length");
    }
    uint8_t len_bytes[4] = {};
    status = file.read_exact(len_bytes, sizeof(len_bytes));
    if (!status.ok()) return status;
    const uint32_t length = static_cast<uint32_t>(len_bytes[0]) |
                            (static_cast<uint32_t>(len_bytes[1]) << 8) |
                            (static_cast<uint32_t>(len_bytes[2]) << 16) |
                            (static_cast<uint32_t>(len_bytes[3]) << 24);
    if (length > options.limits.max_string_length) {
      return Status::error(ErrorCode::LimitExceeded,
                           "metadata string too long");
    }
    if (length > file.remaining()) {
      return Status::error(ErrorCode::Truncated, "truncated metadata string");
    }
    std::string text(length, '\0');
    status = file.read_exact(length == 0 ? nullptr : text.data(), length);
    if (!status.ok()) return status;
    switch (i) {
      case 0: out.meta.app_name = std::move(text); break;
      case 1: out.meta.app_version = std::move(text); break;
      case 2: out.meta.exe_path = std::move(text); break;
      case 3: out.meta.arch = std::move(text); break;
      case 4: out.meta.os = std::move(text); break;
      default: break;
    }
  }

  bool saw_footer = false;
  uint64_t footer_chunks = 0;
  uint64_t footer_records = 0;
  uint64_t footer_duration = 0;
  std::vector<uint32_t> local_to_global;
  std::vector<uint8_t> buffer;
  while (file.remaining() > 0) {
    if (file.remaining() < 8) {
      auto recovery_status = recovery("trailing partial record");
      if (!recovery_status.ok()) return recovery_status;
      break;
    }
    std::vector<uint8_t> magic(8);
    status = file.read_exact(magic, magic.size());
    if (!status.ok()) return status;
    if (std::memcmp(magic.data(), kFooterMagic.data(), 8) == 0) {
      if (file.remaining() < 28) {
        auto recovery_status = recovery("partial footer");
        if (!recovery_status.ok()) return recovery_status;
        break;
      }
      status = file.read_exact(buffer, 28);
      if (!status.ok()) return status;
      const uint32_t stored_crc = static_cast<uint32_t>(buffer[24]) |
                                  (static_cast<uint32_t>(buffer[25]) << 8) |
                                  (static_cast<uint32_t>(buffer[26]) << 16) |
                                  (static_cast<uint32_t>(buffer[27]) << 24);
      if (trace::crc32(std::span<const uint8_t>(buffer.data(), 24)) !=
          stored_crc) {
        return corrupt("footer checksum mismatch");
      }
      ByteReader footer_tail(std::span<const uint8_t>(buffer.data(), 24));
      footer_tail.u64(footer_chunks);
      footer_tail.u64(footer_records);
      footer_tail.u64(footer_duration);
      saw_footer = true;
      break;
    }
    if (std::memcmp(magic.data(), kChunkMagic.data(), 8) != 0) {
      auto recovery_status = recovery("unknown bytes after chunks");
      if (!recovery_status.ok()) return recovery_status;
      break;
    }
    if (file.remaining() < 12) {
      auto recovery_status = recovery("partial chunk header");
      if (!recovery_status.ok()) return recovery_status;
      break;
    }
    status = file.read_exact(buffer, 12);
    if (!status.ok()) return status;
    ByteReader chunk_header(std::span<const uint8_t>(buffer.data(), 12));
    uint32_t payload_len = 0;
    uint32_t stored_crc = 0;
    uint32_t chunk_records = 0;
    chunk_header.u32(payload_len);
    chunk_header.u32(stored_crc);
    chunk_header.u32(chunk_records);
    if (payload_len > kMaxChunkPayload) {
      return Status::error(ErrorCode::LimitExceeded, "chunk payload too large");
    }
    if (file.remaining() < payload_len) {
      auto recovery_status = recovery("truncated chunk payload");
      if (!recovery_status.ok()) return recovery_status;
      break;
    }
    status = file.read_exact(buffer, payload_len);
    if (!status.ok()) return status;
    const std::span<const uint8_t> payload(buffer.data(), buffer.size());
    if (options.verify_checksums && trace::crc32(payload) != stored_crc) {
      return corrupt("chunk checksum mismatch");
    }
    if (out.chunks.size() >= options.limits.max_chunks) {
      return Status::error(ErrorCode::LimitExceeded, "chunk count limit");
    }
    ByteReader payload_reader(payload);
    uint32_t decoded_records = 0;
    const size_t first_record = out.records.size();
    status = decode_chunk(payload_reader, options.limits, out,
                          local_to_global, decoded_records);
    if (!status.ok()) return status;
    if (decoded_records != chunk_records) {
      return corrupt("chunk record count mismatch");
    }
    ChunkInfo info;
    info.file_offset =
        file.offset() - payload_len - static_cast<uint64_t>(kChunkHeaderSize);
    info.record_count = decoded_records;
    if (first_record < out.records.size()) {
      info.first_ts = out.records[first_record].ts;
      info.last_ts = info.first_ts;
      for (size_t i = first_record; i < out.records.size(); ++i) {
        info.first_ts = std::min(info.first_ts, out.records[i].ts);
        info.last_ts = std::max(info.last_ts, out.records[i].ts);
      }
    }
    out.chunks.push_back(info);
  }

  if (!saw_footer) {
    auto recovery_status = recovery(path + ": missing footer");
    if (!recovery_status.ok()) return recovery_status;
  }
  if (saw_footer && file.remaining() > 0) {
    return corrupt("trailing bytes after footer");
  }
  if (saw_footer) {
    if (footer_chunks != out.chunks.size() ||
        footer_records != out.records.size()) {
      return corrupt("footer does not match decoded content");
    }
    if (!out.records.empty() && footer_duration < out.records.back().ts) {
      return corrupt("footer duration precedes the last record");
    }
  }
  out.meta.recorded_ns =
      saw_footer ? footer_duration
                 : (out.records.empty() ? 0 : out.records.back().ts);
  out.diagnostics.bytes_read = static_cast<size_t>(file.size());
  out.diagnostics.streamed = true;
  out.finalize();
  return Status::success();
}

}  // namespace

Status load_trace(const std::string& path, Trace& out,
                  const LoadOptions& options) {
  try {
    if (options.streaming) return load_streamed(path, out, options);
    std::vector<uint8_t> bytes;
    auto status = trace::read_file(path, options.limits.max_file_bytes, bytes);
    if (!status.ok()) return status;
    return load_trace_bytes(bytes, out, options, path);
  } catch (const std::bad_alloc&) {
    return Status::error(ErrorCode::LimitExceeded,
                         path + ": out of memory while decoding");
  } catch (const std::exception& e) {
    return Status::error(ErrorCode::Internal,
                         path + ": " + std::string(e.what()));
  }
}

Status load_trace_bytes(std::span<const uint8_t> bytes, Trace& out,
                        const LoadOptions& options, const std::string& source) {
  try {
    return load_impl(bytes, out, options, source);
  } catch (const std::bad_alloc&) {
    return Status::error(ErrorCode::LimitExceeded,
                         source + ": out of memory while decoding");
  } catch (const std::exception& e) {
    return Status::error(ErrorCode::Internal,
                         source + ": " + std::string(e.what()));
  }
}

}  // namespace gbx
