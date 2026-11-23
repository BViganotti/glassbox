// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "glassbox/attributes.hpp"
#include "glassbox/status.hpp"
#include "glassbox/trace.hpp"
#include "glassbox/trace_io.hpp"

namespace gbx {

/// What to do when the bounded producer queue is full.
enum class DropPolicy {
  /// Drop the incoming record and account for it. Never blocks the host.
  DropNewest,
  /// Block the producer until the writer catches up. Can stall a real-time
  /// application; use only when losing records is unacceptable.
  Block,
};

struct RecorderConfig {
  size_t ring_capacity = 8192;  // pending records, clamped to [64, 262144]
  size_t chunk_bytes = 1u << 20;
  uint32_t chunk_records = 50000;
  DropPolicy drop_policy = DropPolicy::DropNewest;
  size_t max_strings = 262144;
  size_t max_string_bytes = 32u << 20;
  bool sync_on_close = false;
};

/// Accounting for a recorder session. Dropped records are always reported:
/// Glassbox never silently loses data.
struct RecorderStats {
  uint64_t records_written = 0;
  uint64_t records_dropped = 0;
  uint64_t records_rejected = 0;
  uint64_t queue_full_events = 0;
  uint64_t strings_dropped = 0;
  uint64_t spans_started = 0;
  uint64_t spans_ended = 0;
  uint64_t chunks = 0;
  uint64_t bytes_written = 0;
  size_t pending = 0;
  size_t queue_capacity = 0;
  bool recording = false;
  bool failed = false;
  std::string last_error;
};

/// One field of a live event, with strings already resolved so consumers do
/// not need access to the recorder's string table.
struct LiveField {
  std::string key;
  Value value;
  std::string text;  // resolved string value, or empty
};

struct LiveRecord {
  uint64_t ts = 0;
  RecordType type = RecordType::Marker;
  uint32_t thread = 0;
  std::string thread_name;
  std::vector<LiveField> fields;
};

/// Receives records as the writer drains them. Called from the recorder's
/// writer thread, one at a time, never while internal locks are held. A slow
/// sink slows the recording; it must not throw.
using LiveSink = std::function<void(const LiveRecord&)>;

class Span;

/// A bounded, multi-producer trace recorder.
///
/// Thread-safety: every recording method may be called from any thread
/// concurrently. `start`, `finish`, `cancel` and `set_live_sink` must not race
/// with each other (they may race with recording calls). A single recorder
/// owns one output file and one writer thread.
///
/// Lifetime: `finish()` (or the destructor) flushes and closes the trace.
/// Recording calls after finish are rejected and accounted in the stats.
class Recorder {
 public:
  explicit Recorder(RecorderConfig config = {});
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  /// Opens `path` and starts the writer thread. `meta` defaults to the current
  /// process metadata. Returns an error if already recording.
  Status start(const std::string& path, TraceMeta meta);

  /// Starts with automatically collected process metadata.
  Status start(const std::string& path);

  /// Drains the queue, writes the footer and joins the writer. Safe to call
  /// when not recording (returns the previous result).
  Status finish();

  /// Stops recording immediately. The partial file has no footer and will be
  /// reported as truncated when opened. Idempotent.
  void cancel();

  bool is_recording() const;

  /// Installs a live-stream sink. May be called before or during recording.
  void set_live_sink(LiveSink sink);

  // --- recording API -------------------------------------------------------
  void log(LogLevel level, std::string_view message,
           std::string_view category = {});
  void counter(std::string_view name, double value, std::string_view unit = {});
  void gauge(std::string_view name, double value);
  void mark(std::string_view label);
  void snapshot(std::initializer_list<std::pair<std::string_view, double>> kv);
  void snapshot_text(
      std::initializer_list<std::pair<std::string_view, std::string_view>> kv);
  void alloc(const void* ptr, size_t size, std::string_view site,
             std::string_view klass = {}, const void* owner = nullptr);
  void free(const void* ptr);
  void sample(std::span<const std::string_view> frames, double weight = 1.0);
  void thread_name(std::string_view name);

  /// Opens a span. `parent` is the enclosing span id; 0 means "the innermost
  /// open span on this thread". Spans are not required to be strictly nested;
  /// the bookkeeping tolerates unbalanced ends.
  uint32_t span_begin(std::string_view name, std::string_view kind = {},
                      uint32_t parent = 0);
  void span_end(uint32_t id);

  /// RAII span that ends on destruction.
  Span begin_span(std::string_view name, std::string_view kind = {});

  /// Innermost open span on the calling thread for this recorder, or 0.
  uint32_t current_span() const;

  /// Dense thread slot assigned to the calling thread, or 0 before start.
  uint32_t thread_slot() const;

  RecorderStats stats() const;

  /// Error that stopped the writer, if any. Dropped-record accounting is not
  /// an error; read stats() for that.
  Status error() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Convenience macros. They evaluate the recorder expression once.
#define GBX_SPAN(recorder, name) \
  auto gbx_span_ = (recorder).begin_span((name))
#define GBX_LOGF(recorder, level, message) \
  (recorder).log((level), (message))

}  // namespace gbx
