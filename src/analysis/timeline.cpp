// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "glassbox/analysis.hpp"

namespace gbx {
namespace {

constexpr uint32_t kMaxBuckets = 512;

}  // namespace

Timeline build_timeline(const Trace& trace, uint32_t bucket_count,
                        TimeWindow window) {
  Timeline timeline;
  bucket_count = std::clamp<uint32_t>(bucket_count, 1, kMaxBuckets);
  const uint64_t trace_end = trace.end_ts();
  uint64_t begin = window.begin;
  uint64_t end = window.end == 0 ? trace_end : window.end;
  if (end <= begin) end = begin + 1;
  timeline.begin_ts = begin;
  timeline.end_ts = end;
  const uint64_t span = end - begin;
  const uint64_t width = std::max<uint64_t>(1, (span + bucket_count - 1) / bucket_count);
  timeline.bucket_width = width;

  timeline.buckets.resize(bucket_count);
  for (uint32_t i = 0; i < bucket_count; ++i) {
    auto& bucket = timeline.buckets[i];
    bucket.begin_ts = begin + static_cast<uint64_t>(i) * width;
    bucket.end_ts = std::min<uint64_t>(begin + (static_cast<uint64_t>(i) + 1) * width,
                                       end);
  }

  auto bucket_for = [&](uint64_t ts) -> TimelineBucket* {
    if (ts < begin || ts > end) return nullptr;
    const uint64_t index = std::min<uint64_t>((ts - begin) / width,
                                              bucket_count - 1);
    return &timeline.buckets[static_cast<size_t>(index)];
  };

  const auto [first, last] = trace.record_range(begin, end);
  for (size_t i = first; i < last; ++i) {
    const auto& record = trace.records[i];
    TimelineBucket* bucket = bucket_for(record.ts);
    if (bucket == nullptr) continue;
    switch (record.type) {
      case RecordType::Alloc: {
        const int64_t size = trace.int_field(record, keys::kSize, 0);
        if (size > 0) {
          const auto bytes = static_cast<uint64_t>(size);
          bucket->alloc_bytes += bytes;
          bucket->alloc_count += 1;
          timeline.total_alloc_bytes += bytes;
        }
        break;
      }
      case RecordType::Free:
        bucket->free_count += 1;
        break;
      case RecordType::Log: {
        bucket->log_count += 1;
        timeline.total_logs += 1;
        const int64_t level = trace.int_field(record, keys::kLevel, 0);
        if (level >= static_cast<int64_t>(LogLevel::Error)) {
          bucket->error_count += 1;
          timeline.total_errors += 1;
        }
        break;
      }
      case RecordType::Sample:
        bucket->sample_count += 1;
        timeline.total_samples += 1;
        break;
      case RecordType::SpanBegin:
        bucket->span_starts += 1;
        break;
      default:
        break;
    }
  }

  // Span durations are distributed across every bucket they overlap, so the
  // timeline communicates concurrency (the sum can exceed wall time).
  const SpanAnalysis spans = analyze_spans(trace);
  for (const auto& info : spans.spans) {
    const uint64_t span_end = info.closed ? info.end_ts : trace_end;
    const uint64_t overlap_begin = std::max(info.begin_ts, begin);
    const uint64_t overlap_end = std::min(span_end, end);
    if (overlap_end <= overlap_begin) continue;
    const size_t first_bucket = static_cast<size_t>(
        std::min<uint64_t>((overlap_begin - begin) / width, bucket_count - 1));
    const size_t last_bucket = static_cast<size_t>(
        std::min<uint64_t>((overlap_end - begin) / width, bucket_count - 1));
    for (size_t b = first_bucket; b <= last_bucket; ++b) {
      auto& bucket = timeline.buckets[b];
      const uint64_t lo = std::max(overlap_begin, bucket.begin_ts);
      const uint64_t hi = std::min(overlap_end, bucket.end_ts);
      if (hi <= lo) continue;
      bucket.span_ns += static_cast<double>(hi - lo);
    }
  }

  for (const auto& bucket : timeline.buckets) {
    timeline.max_span_ns = std::max(timeline.max_span_ns, bucket.span_ns);
    timeline.max_alloc_bytes =
        std::max(timeline.max_alloc_bytes, bucket.alloc_bytes);
  }
  return timeline;
}

}  // namespace gbx
