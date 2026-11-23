// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/recorder.hpp"

#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "alloc/hook_suppress.hpp"
#include "glassbox/span.hpp"
#include "trace/trace_writer.hpp"

namespace gbx {
namespace {

constexpr size_t kMaxPendingFields = 24;
constexpr uint32_t kMaxThreads = 65535;
constexpr size_t kMaxFrames = 64;
constexpr size_t kMaxFrameBytes = 1024;
constexpr size_t kMaxFramesBytes = 8 * 1024;

std::atomic<uint64_t> g_instance_counter{1};

uint64_t monotonic_ns() {
  struct timespec ts {};
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

std::string default_thread_name(unsigned slot) {
  char buffer[64] = {};
  if (pthread_getname_np(pthread_self(), buffer, sizeof(buffer)) == 0 &&
      buffer[0] != '\0') {
    return std::string(buffer);
  }
  return "Thread " + std::to_string(slot);
}

struct FieldBuf {
  Field fields[kMaxPendingFields];
  size_t count = 0;
  bool ok = true;

  void add(uint32_t key, Value value) {
    if (count >= kMaxPendingFields) {
      ok = false;
      return;
    }
    fields[count++] = Field{key, value};
  }
};

struct Pending {
  Record record;
  Field fields[kMaxPendingFields];
};

struct ThreadContext {
  uint64_t instance = 0;
  uint32_t slot = 0;
  bool named = false;
  std::vector<uint32_t> spans;

  uint32_t top() const { return spans.empty() ? 0 : spans.back(); }
};

thread_local ThreadContext g_thread_context;

}  // namespace

class Recorder::Impl {
 public:
  explicit Impl(RecorderConfig config) : config_(normalize(config)) {
    ring_.resize(config_.ring_capacity);
    stats_.queue_capacity = config_.ring_capacity;
  }

  ~Impl() {
    if (recording_.load()) {
      finish();
    }
  }

  Status start(const std::string& path, TraceMeta meta) {
    detail::AllocHookSuppress suppress;
    std::lock_guard lifecycle(lifecycle_mu_);
    if (recording_.load()) {
      return Status::error(ErrorCode::Busy, "recorder already recording");
    }
    if (used_) {
      return Status::error(ErrorCode::Busy, "recorder has already been used");
    }
    instance_ = g_instance_counter.fetch_add(1);
    {
      std::lock_guard lock(intern_mu_);
      strings_ = StringTable(
          StringTableLimits{config_.max_strings, config_.max_string_bytes,
                            64u << 10});
      warn_name_id_ = strings_.try_intern("recorder_warnings").value_or(
          StringTable::kInvalidId);
      warn_dropped_id_ =
          strings_.try_intern("dropped_records").value_or(
              StringTable::kInvalidId);
      warn_strings_id_ =
          strings_.try_intern("dropped_strings").value_or(
              StringTable::kInvalidId);
      warn_rejected_id_ =
          strings_.try_intern("rejected_records").value_or(
              StringTable::kInvalidId);
    }
    {
      std::lock_guard lock(mu_);
      stats_ = RecorderStats{};
      stats_.queue_capacity = config_.ring_capacity;
      head_ = tail_ = count_ = 0;
      write_error_ = Status::success();
      finish_status_ = Status::success();
      finished_ = false;
      failed_.store(false);
      cancelled_.store(false);
      stopping_.store(false);
    }
    SaveOptions options;
    options.chunk_bytes = config_.chunk_bytes;
    options.chunk_records = config_.chunk_records;
    options.sync_on_close = config_.sync_on_close;
    auto status = writer_.open(path, meta, options);
    if (!status.ok()) return status;
    start_ns_.store(monotonic_ns());
    recording_.store(true);
    used_ = true;
    writer_thread_ = std::thread([this] { writer_loop(); });
    return Status::success();
  }

  Status finish() {
    detail::AllocHookSuppress suppress;
    std::thread thread;
    {
      std::lock_guard lifecycle(lifecycle_mu_);
      if (!recording_.load()) {
        std::lock_guard lock(mu_);
        return finished_ ? finish_status_ : Status::success();
      }
      stopping_.store(true);
      cv_.notify_all();
      space_cv_.notify_all();
      thread = std::move(writer_thread_);
    }
    if (thread.joinable()) thread.join();
    {
      std::lock_guard lifecycle(lifecycle_mu_);
      recording_.store(false);
      std::lock_guard lock(mu_);
      finished_ = true;
      stats_.recording = false;
      stats_.pending = 0;
      return finish_status_;
    }
  }

  void cancel() {
    detail::AllocHookSuppress suppress;
    std::thread thread;
    {
      std::lock_guard lifecycle(lifecycle_mu_);
      cancelled_.store(true);
      stopping_.store(true);
      cv_.notify_all();
      space_cv_.notify_all();
      thread = std::move(writer_thread_);
    }
    if (thread.joinable()) thread.join();
    writer_.abort();
    std::lock_guard lifecycle(lifecycle_mu_);
    recording_.store(false);
    std::lock_guard lock(mu_);
    finished_ = true;
    record_dropped_locked();
  }

  bool is_recording() const { return recording_.load(); }

  void set_live_sink(LiveSink sink) {
    std::lock_guard lock(sink_mu_);
    sink_ = std::move(sink);
  }

  RecorderStats stats() const {
    std::lock_guard lock(mu_);
    RecorderStats copy = stats_;
    copy.pending = count_;
    copy.recording = recording_.load();
    copy.failed = failed_.load();
    return copy;
  }

  Status error() const {
    std::lock_guard lock(mu_);
    return write_error_;
  }

  uint64_t now_ts() const {
    if (!recording_.load()) return 0;
    const uint64_t start = start_ns_.load();
    const uint64_t now = monotonic_ns();
    return now >= start ? now - start : 0;
  }

  std::optional<uint32_t> intern(std::string_view text) {
    std::lock_guard lock(intern_mu_);
    return strings_.try_intern(text);
  }

  uint32_t thread_slot() {
    detail::AllocHookSuppress suppress;
    if (!recording_.load()) return 0;
    auto& ctx = thread_context();
    if (ctx.slot == 0) {
      const uint32_t slot = next_thread_slot_.fetch_add(1);
      if (slot >= kMaxThreads) {
        reject();
        return 0;
      }
      ctx.slot = slot;
      emit_thread_name(slot, default_thread_name(slot));
    }
    return ctx.slot;
  }

  void thread_name(std::string_view name) {
    detail::AllocHookSuppress suppress;
    if (!recording_.load()) return;
    if (name.empty()) return;
    const uint32_t slot = thread_slot();
    if (slot == 0) return;
    emit_thread_name(slot, std::string(name));
  }

  uint32_t current_span() {
    if (!recording_.load()) return 0;
    if (thread_context().instance != instance_.load()) return 0;
    return g_thread_context.top();
  }

  // --- emission ------------------------------------------------------------
  void log(LogLevel level, std::string_view message,
           std::string_view category) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    FieldBuf fields;
    fields.add(intern_required(keys::kLevel),
               Value::integer(static_cast<int64_t>(
                   std::min<uint8_t>(static_cast<uint8_t>(level),
                                     static_cast<uint8_t>(LogLevel::Fatal)))));
    fields.add(intern_required(keys::kMsg), string_value(message, fields));
    if (!category.empty()) {
      fields.add(intern_required(keys::kCategory),
                 string_value(category, fields));
    }
    emit(now_ts(), RecordType::Log, slot, fields);
  }

  void counter(std::string_view name, double value, std::string_view unit) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    FieldBuf fields;
    fields.add(intern_required(keys::kName), string_value(name, fields));
    fields.add(intern_required(keys::kValue), Value::real(value));
    if (!unit.empty()) {
      fields.add(intern_required(keys::kUnit), string_value(unit, fields));
    }
    emit(now_ts(), RecordType::Counter, slot, fields);
  }

  void gauge(std::string_view name, double value) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    FieldBuf fields;
    fields.add(intern_required(keys::kName), string_value(name, fields));
    fields.add(intern_required(keys::kValue), Value::real(value));
    emit(now_ts(), RecordType::Gauge, slot, fields);
  }

  void mark(std::string_view label) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    FieldBuf fields;
    fields.add(intern_required(keys::kLabel), string_value(label, fields));
    emit(now_ts(), RecordType::Marker, slot, fields);
  }

  void alloc(const void* ptr, size_t size, std::string_view site,
             std::string_view klass, const void* owner) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    FieldBuf fields;
    fields.add(intern_required(keys::kPtr), Value::pointer(ptr));
    fields.add(intern_required(keys::kSize),
               Value::integer(static_cast<int64_t>(size)));
    fields.add(intern_required(keys::kSite), string_value(site, fields));
    if (!klass.empty()) {
      fields.add(intern_required(keys::kClass), string_value(klass, fields));
    }
    if (owner != nullptr) {
      fields.add(intern_required(keys::kOwner), Value::pointer(owner));
    }
    emit(now_ts(), RecordType::Alloc, slot, fields);
  }

  void free(const void* ptr) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    FieldBuf fields;
    fields.add(intern_required(keys::kPtr), Value::pointer(ptr));
    emit(now_ts(), RecordType::Free, slot, fields);
  }

  void sample(std::span<const std::string_view> frames, double weight) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    FieldBuf fields;
    std::string joined;
    size_t used = 0;
    bool truncated = false;
    for (size_t i = 0; i < frames.size(); ++i) {
      if (i >= kMaxFrames || used >= kMaxFramesBytes) {
        truncated = true;
        break;
      }
      std::string_view frame = frames[i];
      if (frame.size() > kMaxFrameBytes) {
        frame = frame.substr(0, kMaxFrameBytes);
        truncated = true;
      }
      if (used + frame.size() + 1 > kMaxFramesBytes) {
        truncated = true;
        break;
      }
      if (!joined.empty()) joined.push_back(kFrameSeparator);
      for (char c : frame) {
        joined.push_back(c == kFrameSeparator ? '?' : c);
      }
      used += frame.size() + 1;
    }
    fields.add(intern_required(keys::kFrames), string_value(joined, fields));
    fields.add(intern_required(keys::kWeight), Value::real(weight));
    if (truncated) {
      fields.add(intern_required("truncated"), Value::boolean(true));
    }
    emit(now_ts(), RecordType::Sample, slot, fields);
  }

  void snapshot(std::initializer_list<std::pair<std::string_view, double>> kv) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    const uint64_t ts = now_ts();
    std::vector<Field> fields;
    fields.reserve(kv.size());
    for (const auto& [key, value] : kv) {
      auto key_id = intern(key);
      if (!key_id) {
        drop_for_string_limit();
        return;
      }
      fields.push_back({*key_id, Value::real(value)});
    }
    emit_snapshot(ts, slot, fields);
  }

  void snapshot_text(
      std::initializer_list<std::pair<std::string_view, std::string_view>> kv) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    const uint64_t ts = now_ts();
    std::vector<Field> fields;
    fields.reserve(kv.size());
    for (const auto& [key, text] : kv) {
      auto key_id = intern(key);
      auto value_id = intern(text);
      if (!key_id || !value_id) {
        drop_for_string_limit();
        return;
      }
      fields.push_back({*key_id, Value::str(*value_id)});
    }
    emit_snapshot(ts, slot, fields);
  }

  uint32_t span_begin(std::string_view name, std::string_view kind,
                      uint32_t parent) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return 0;
    auto slot = thread_slot();
    if (slot == 0) return 0;
    auto& ctx = thread_context();
    const uint32_t id = next_span_id_.fetch_add(1);
    if (id == 0) return 0;
    if (parent == 0) parent = ctx.top();
    FieldBuf fields;
    fields.add(intern_required(keys::kId), Value::integer(id));
    fields.add(intern_required(keys::kParent), Value::integer(parent));
    fields.add(intern_required(keys::kName), string_value(name, fields));
    if (!kind.empty()) {
      fields.add(intern_required(keys::kKind), string_value(kind, fields));
    }
    ctx.spans.push_back(id);
    {
      std::lock_guard lock(mu_);
      ++stats_.spans_started;
    }
    emit(now_ts(), RecordType::SpanBegin, slot, fields);
    return id;
  }

  void span_end(uint32_t id) {
    detail::AllocHookSuppress suppress;
    if (!begin()) return;
    auto slot = thread_slot();
    if (slot == 0) return;
    auto& ctx = thread_context();
    uint32_t parent = 0;
    auto it = std::find(ctx.spans.rbegin(), ctx.spans.rend(), id);
    if (it != ctx.spans.rend()) {
      const auto index = static_cast<size_t>(std::distance(ctx.spans.begin(), it.base()) - 1);
      parent = index > 0 ? ctx.spans[index - 1] : 0;
      ctx.spans.erase(ctx.spans.begin() + static_cast<ptrdiff_t>(index));
    }
    FieldBuf fields;
    fields.add(intern_required(keys::kId), Value::integer(id));
    {
      std::lock_guard lock(mu_);
      ++stats_.spans_ended;
    }
    const uint64_t ts = now_ts();
    emit_with_span(ts, RecordType::SpanEnd, slot, parent, fields);
  }

  Status start_path(const std::string& path) {
    return start(path, current_meta("glassbox", GBX_VERSION));
  }

  size_t capacity() const { return config_.ring_capacity; }

 private:
  static RecorderConfig normalize(RecorderConfig config) {
    config.ring_capacity =
        std::clamp<size_t>(config.ring_capacity, 64, 262144);
    if (config.chunk_bytes == 0) config.chunk_bytes = 1u << 20;
    if (config.chunk_records == 0) config.chunk_records = 50000;
    if (config.max_strings < 16) config.max_strings = 16;
    if (config.max_string_bytes < 1024) config.max_string_bytes = 1024;
    return config;
  }

  ThreadContext& thread_context() {
    if (g_thread_context.instance != instance_.load()) {
      g_thread_context = ThreadContext{};
      g_thread_context.instance = instance_.load();
    }
    return g_thread_context;
  }

  bool begin() {
    if (!recording_.load()) {
      std::lock_guard lock(mu_);
      ++stats_.records_rejected;
      return false;
    }
    return true;
  }

  void reject() {
    std::lock_guard lock(mu_);
    ++stats_.records_rejected;
  }

  void drop_record() {
    std::lock_guard lock(mu_);
    ++stats_.records_dropped;
  }

  void drop_for_string_limit() {
    std::lock_guard lock(mu_);
    ++stats_.records_dropped;
    ++stats_.strings_dropped;
  }

  uint32_t intern_required(std::string_view text) {
    auto id = intern(text);
    if (!id) {
      std::lock_guard lock(mu_);
      ++stats_.strings_dropped;
      return StringTable::kInvalidId;
    }
    return *id;
  }

  Value string_value(std::string_view text, FieldBuf& fields) {
    auto id = intern(text);
    if (!id) {
      fields.ok = false;
      std::lock_guard lock(mu_);
      ++stats_.strings_dropped;
      return Value::str(StringTable::kInvalidId);
    }
    return Value::str(*id);
  }

  void emit_thread_name(uint32_t slot, const std::string& name) {
    FieldBuf fields;
    fields.add(intern_required(keys::kName), string_value(name, fields));
    {
      std::lock_guard lock(mu_);
      if (thread_names_.size() <= slot) thread_names_.resize(slot + 1);
      thread_names_[slot] = name;
    }
    Pending pending{};
    pending.record.ts = now_ts();
    pending.record.type = RecordType::ThreadName;
    pending.record.thread = slot;
    pending.record.span = 0;
    pending.record.field_count =
        static_cast<uint16_t>(std::min(fields.count, kMaxPendingFields));
    std::memcpy(pending.fields, fields.fields,
                pending.record.field_count * sizeof(Field));
    push(pending);
  }

  void emit(uint64_t ts, RecordType type, uint32_t slot, FieldBuf& fields) {
    emit_with_span(ts, type, slot, thread_context().top(), fields);
  }

  void emit_with_span(uint64_t ts, RecordType type, uint32_t slot,
                      uint32_t span, FieldBuf& fields) {
    if (!fields.ok || fields.count > kMaxPendingFields) {
      drop_record();
      return;
    }
    for (size_t i = 0; i < fields.count; ++i) {
      const auto& field = fields.fields[i];
      if (field.key == StringTable::kInvalidId ||
          field.value.type < ValueType::Int ||
          field.value.type > ValueType::Bool ||
          (field.value.type == ValueType::String &&
           field.value.s == StringTable::kInvalidId)) {
        drop_record();
        return;
      }
    }
    Pending pending{};
    pending.record.ts = ts;
    pending.record.type = type;
    pending.record.thread = slot;
    pending.record.span = span;
    pending.record.field_count = static_cast<uint16_t>(fields.count);
    std::memcpy(pending.fields, fields.fields, fields.count * sizeof(Field));
    push(pending);
  }

  void emit_snapshot(uint64_t ts, uint32_t slot,
                     const std::vector<Field>& fields) {
    size_t offset = 0;
    while (offset < fields.size()) {
      const size_t count =
          std::min(kMaxPendingFields, fields.size() - offset);
      Pending pending{};
      pending.record.ts = ts;
      pending.record.type = RecordType::Snapshot;
      pending.record.thread = slot;
      pending.record.span = thread_context().top();
      pending.record.field_count = static_cast<uint16_t>(count);
      std::memcpy(pending.fields, fields.data() + offset,
                  count * sizeof(Field));
      if (!push(pending)) return;
      offset += count;
    }
    if (fields.empty()) {
      Pending pending{};
      pending.record.ts = ts;
      pending.record.type = RecordType::Snapshot;
      pending.record.thread = slot;
      pending.record.span = thread_context().top();
      pending.record.field_count = 0;
      push(pending);
    }
  }

  bool push(const Pending& pending) {
    std::unique_lock lock(mu_);
    if (count_ >= ring_.size()) {
      ++stats_.queue_full_events;
      if (config_.drop_policy == DropPolicy::Block && !stopping_.load() &&
          !failed_.load()) {
        space_cv_.wait(lock, [this] {
          return count_ < ring_.size() || stopping_.load() || failed_.load();
        });
      }
      if (count_ >= ring_.size()) {
        ++stats_.records_dropped;
        return false;
      }
    }
    ring_[tail_] = pending;
    tail_ = (tail_ + 1) % ring_.size();
    ++count_;
    cv_.notify_one();
    return true;
  }

  void record_dropped_locked() { ++stats_.records_dropped; }

  void writer_loop() {
    detail::AllocHookSuppress suppress;
    pthread_setname_np("gbx-writer");
    while (true) {
      Pending pending{};
      bool have = false;
      {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [this] { return count_ > 0 || stopping_.load(); });
        if (count_ > 0) {
          pending = ring_[head_];
          head_ = (head_ + 1) % ring_.size();
          --count_;
          have = true;
          space_cv_.notify_all();
        } else if (stopping_.load()) {
          break;
        }
      }
      if (!have) break;
      if (cancelled_.load()) {
        std::lock_guard lock(mu_);
        record_dropped_locked();
        continue;
      }
      write_pending(pending);
    }
    finalize_writer();
  }

  void write_pending(const Pending& pending) {
    if (failed_.load()) {
      std::lock_guard lock(mu_);
      ++stats_.records_dropped;
      return;
    }
    const auto fields = std::span<const Field>(pending.fields,
                                              pending.record.field_count);
    Status status;
    {
      std::lock_guard lock(intern_mu_);
      status = writer_.append(pending.record, fields, strings_);
    }
    if (!status.ok()) {
      std::lock_guard lock(mu_);
      if (write_error_.ok()) write_error_ = status;
      failed_.store(true);
      ++stats_.records_dropped;
      space_cv_.notify_all();
      return;
    }
    LiveSink sink;
    {
      std::lock_guard lock(sink_mu_);
      sink = sink_;
    }
    if (sink) invoke_sink(pending, sink);
    std::lock_guard lock(mu_);
    ++stats_.records_written;
    stats_.chunks = writer_.chunk_count();
    stats_.bytes_written = writer_.offset();
  }

  void invoke_sink(const Pending& pending, const LiveSink& sink) {
    LiveRecord live;
    live.ts = pending.record.ts;
    live.type = pending.record.type;
    live.thread = pending.record.thread;
    {
      std::lock_guard lock(mu_);
      if (pending.record.thread < thread_names_.size()) {
        live.thread_name = thread_names_[pending.record.thread];
      }
    }
    live.fields.reserve(pending.record.field_count);
    {
      std::lock_guard lock(intern_mu_);
      for (size_t i = 0; i < pending.record.field_count; ++i) {
        const Field& f = pending.fields[i];
        LiveField out;
        out.key = std::string(strings_.get(f.key));
        out.value = f.value;
        if (f.value.type == ValueType::String) {
          out.text = std::string(strings_.get(f.value.s));
        }
        live.fields.push_back(std::move(out));
      }
    }
    try {
      sink(live);
    } catch (...) {
      // A misbehaving sink must never take down the recorder.
    }
  }

  void finalize_writer() {
    if (cancelled_.load()) {
      Status status = writer_.abort();
      std::lock_guard lock(mu_);
      finish_status_ = status;
      stats_.chunks = writer_.chunk_count();
      stats_.bytes_written = writer_.offset();
      space_cv_.notify_all();
      return;
    }
    bool ok = !failed_.load();
    uint64_t dropped = 0;
    uint64_t rejected = 0;
    uint64_t strings_dropped = 0;
    {
      std::lock_guard lock(mu_);
      dropped = stats_.records_dropped;
      rejected = stats_.records_rejected;
      strings_dropped = stats_.strings_dropped;
    }
    if (ok && dropped > 0) {
      // Persist the loss accounting inside the trace itself. These keys were
      // reserved at startup so they survive an exhausted string table.
      FieldBuf fields;
      if (warn_dropped_id_ != StringTable::kInvalidId) {
        fields.add(warn_dropped_id_,
                   Value::integer(static_cast<int64_t>(dropped)));
      }
      if (warn_strings_id_ != StringTable::kInvalidId) {
        fields.add(warn_strings_id_,
                   Value::integer(static_cast<int64_t>(strings_dropped)));
      }
      if (warn_rejected_id_ != StringTable::kInvalidId) {
        fields.add(warn_rejected_id_,
                   Value::integer(static_cast<int64_t>(rejected)));
      }
      Pending pending{};
      pending.record.ts = now_ts();
      pending.record.type = RecordType::Marker;
      pending.record.thread = 0;
      if (warn_name_id_ != StringTable::kInvalidId && fields.count < kMaxPendingFields) {
        fields.add(warn_name_id_, Value::str(warn_name_id_));
      }
      pending.record.field_count = static_cast<uint16_t>(fields.count);
      std::memcpy(pending.fields, fields.fields,
                  fields.count * sizeof(Field));
      Status status;
      {
        std::lock_guard lock(intern_mu_);
        status = writer_.append(
            pending.record,
            std::span<const Field>(pending.fields, pending.record.field_count),
            strings_);
      }
      if (!status.ok()) ok = false;
    }
    Status status = ok ? writer_.finish() : writer_.abort();
    std::lock_guard lock(mu_);
    finish_status_ = status;
    if (!status.ok() && write_error_.ok()) write_error_ = status;
    stats_.chunks = writer_.chunk_count();
    stats_.bytes_written = writer_.offset();
    failed_.store(!status.ok());
    space_cv_.notify_all();
  }

  RecorderConfig config_;
  mutable std::mutex mu_;
  mutable std::mutex intern_mu_;
  mutable std::mutex sink_mu_;
  std::mutex lifecycle_mu_;
  std::condition_variable cv_;
  std::condition_variable space_cv_;

  std::vector<Pending> ring_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
  StringTable strings_;
  std::vector<std::string> thread_names_;

  uint32_t warn_name_id_ = StringTable::kInvalidId;
  uint32_t warn_dropped_id_ = StringTable::kInvalidId;
  uint32_t warn_strings_id_ = StringTable::kInvalidId;
  uint32_t warn_rejected_id_ = StringTable::kInvalidId;

  std::atomic<uint64_t> start_ns_{0};
  std::atomic<uint64_t> instance_{0};
  std::atomic<uint32_t> next_thread_slot_{1};
  std::atomic<uint32_t> next_span_id_{1};
  std::atomic<bool> recording_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> failed_{false};

  trace::TraceWriter writer_;
  std::thread writer_thread_;
  RecorderStats stats_;
  Status write_error_;
  Status finish_status_;
  LiveSink sink_;
  bool used_ = false;
  bool finished_ = false;
};

// ---------------------------------------------------------------------------

Recorder::Recorder(RecorderConfig config) : impl_(new Impl(config)) {}

Recorder::~Recorder() = default;

Status Recorder::start(const std::string& path, TraceMeta meta) {
  return impl_->start(path, std::move(meta));
}

Status Recorder::start(const std::string& path) {
  return impl_->start_path(path);
}

Status Recorder::finish() { return impl_->finish(); }

void Recorder::cancel() { impl_->cancel(); }

bool Recorder::is_recording() const { return impl_->is_recording(); }

void Recorder::set_live_sink(LiveSink sink) {
  impl_->set_live_sink(std::move(sink));
}

void Recorder::log(LogLevel level, std::string_view message,
                   std::string_view category) {
  impl_->log(level, message, category);
}

void Recorder::counter(std::string_view name, double value,
                       std::string_view unit) {
  impl_->counter(name, value, unit);
}

void Recorder::gauge(std::string_view name, double value) {
  impl_->gauge(name, value);
}

void Recorder::mark(std::string_view label) { impl_->mark(label); }

void Recorder::snapshot(
    std::initializer_list<std::pair<std::string_view, double>> kv) {
  impl_->snapshot(kv);
}

void Recorder::snapshot_text(
    std::initializer_list<std::pair<std::string_view, std::string_view>> kv) {
  impl_->snapshot_text(kv);
}

void Recorder::alloc(const void* ptr, size_t size, std::string_view site,
                     std::string_view klass, const void* owner) {
  impl_->alloc(ptr, size, site, klass, owner);
}

void Recorder::free(const void* ptr) { impl_->free(ptr); }

void Recorder::sample(std::span<const std::string_view> frames, double weight) {
  impl_->sample(frames, weight);
}

void Recorder::thread_name(std::string_view name) {
  impl_->thread_name(name);
}

uint32_t Recorder::span_begin(std::string_view name, std::string_view kind,
                              uint32_t parent) {
  return impl_->span_begin(name, kind, parent);
}

void Recorder::span_end(uint32_t id) { impl_->span_end(id); }

Span Recorder::begin_span(std::string_view name, std::string_view kind) {
  return Span(*this, span_begin(name, kind));
}

uint32_t Recorder::current_span() const {
  return const_cast<Impl*>(impl_.get())->current_span();
}

uint32_t Recorder::thread_slot() const {
  return const_cast<Impl*>(impl_.get())->thread_slot();
}

RecorderStats Recorder::stats() const { return impl_->stats(); }

Status Recorder::error() const { return impl_->error(); }

}  // namespace gbx
