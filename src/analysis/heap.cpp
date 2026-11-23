// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analysis/util.hpp"
#include "glassbox/analysis.hpp"

namespace gbx {
namespace {

struct Reconstruction {
  std::vector<HeapAllocation> allocations;
  std::unordered_map<uint64_t, size_t> live;  // ptr -> index in allocations
  uint64_t unknown_frees = 0;
  uint64_t free_count = 0;
  uint64_t peak_bytes = 0;
  uint64_t peak_ts = 0;
  uint64_t live_bytes = 0;
};

Reconstruction reconstruct(const Trace& trace, uint64_t at) {
  Reconstruction out;
  const auto [first, last] = trace.record_range(0, at);
  for (size_t i = first; i < last; ++i) {
    const auto& record = trace.records[i];
    if (record.type == RecordType::Alloc) {
      const int64_t ptr_value = trace.int_field(record, keys::kPtr);
      const int64_t size_value = trace.int_field(record, keys::kSize);
      if (ptr_value <= 0 || size_value < 0) continue;
      const auto ptr = static_cast<uint64_t>(ptr_value);
      const auto size = static_cast<uint64_t>(size_value);
      auto existing = out.live.find(ptr);
      if (existing != out.live.end()) {
        auto& previous = out.allocations[existing->second];
        previous.freed = true;
        previous.free_ts = record.ts;
        out.live_bytes -= std::min(out.live_bytes, previous.size);
        out.live.erase(existing);
        ++out.free_count;
      }
      HeapAllocation allocation;
      allocation.ptr = ptr;
      allocation.size = size;
      allocation.owner = static_cast<uint64_t>(
          std::max<int64_t>(0, trace.int_field(record, keys::kOwner)));
      allocation.alloc_ts = record.ts;
      allocation.site = std::string(trace.str_field(record, keys::kSite));
      allocation.klass = std::string(trace.str_field(record, keys::kClass));
      allocation.alloc_record = static_cast<uint32_t>(i);
      out.live_bytes += size;
      if (out.live_bytes > out.peak_bytes) {
        out.peak_bytes = out.live_bytes;
        out.peak_ts = record.ts;
      }
      const auto index = out.allocations.size();
      out.allocations.push_back(std::move(allocation));
      out.live.emplace(ptr, index);
    } else if (record.type == RecordType::Free) {
      const int64_t ptr_value = trace.int_field(record, keys::kPtr);
      if (ptr_value <= 0) continue;
      const auto ptr = static_cast<uint64_t>(ptr_value);
      auto it = out.live.find(ptr);
      if (it == out.live.end()) {
        ++out.unknown_frees;
        continue;
      }
      auto& allocation = out.allocations[it->second];
      allocation.freed = true;
      allocation.free_ts = record.ts;
      out.live_bytes -= std::min(out.live_bytes, allocation.size);
      out.live.erase(it);
      ++out.free_count;
    }
  }
  return out;
}

std::string site_key(std::string_view site, std::string_view klass) {
  std::string key(site);
  key.push_back('\x1f');
  key.append(klass);
  return key;
}

}  // namespace

HeapAt heap_at(const Trace& trace, uint64_t at, const HeapOptions& options) {
  HeapAt result;
  result.at = at;
  const Reconstruction recon = reconstruct(trace, at);
  result.alloc_count = recon.allocations.size();
  result.free_count = recon.free_count;
  result.unknown_frees = recon.unknown_frees;
  result.peak_bytes = recon.peak_bytes;
  result.peak_ts = recon.peak_ts;

  struct SiteAccumulator {
    HeapSiteStat stat;
    uint64_t age_sum = 0;
  };
  std::unordered_map<std::string, SiteAccumulator> sites;
  std::vector<const HeapAllocation*> live;
  live.reserve(recon.live.size());
  for (const auto& allocation : recon.allocations) {
    auto& acc = sites[site_key(allocation.site, allocation.klass)];
    acc.stat.site = allocation.site;
    acc.stat.klass = allocation.klass;
    acc.stat.alloc_bytes += allocation.size;
    ++acc.stat.alloc_count;
    if (allocation.freed) {
      ++acc.stat.free_count;
      continue;
    }
    live.push_back(&allocation);
    acc.stat.live_bytes += allocation.size;
    ++acc.stat.live_count;
    result.total_bytes += allocation.size;
    acc.age_sum += at > allocation.alloc_ts ? at - allocation.alloc_ts : 0;
  }
  result.object_count = live.size();

  for (auto& [key, acc] : sites) {
    (void)key;
    if (acc.stat.live_count > 0) {
      acc.stat.average_age_ns =
          static_cast<double>(acc.age_sum) /
          static_cast<double>(acc.stat.live_count);
    }
    result.sites.push_back(std::move(acc.stat));
  }
  std::sort(result.sites.begin(), result.sites.end(),
            [](const HeapSiteStat& a, const HeapSiteStat& b) {
              if (a.live_bytes != b.live_bytes) return a.live_bytes > b.live_bytes;
              if (a.site != b.site) return a.site < b.site;
              return a.klass < b.klass;
            });
  if (result.sites.size() > options.max_sites) {
    result.sites_truncated = result.sites.size() - options.max_sites;
    result.sites.resize(options.max_sites);
  }

  std::sort(live.begin(), live.end(),
            [](const HeapAllocation* a, const HeapAllocation* b) {
              if (a->size != b->size) return a->size > b->size;
              return a->ptr < b->ptr;
            });
  const size_t object_count =
      std::min(live.size(), std::max<size_t>(options.max_objects, 1));
  result.objects.reserve(object_count);
  for (size_t i = 0; i < object_count; ++i) {
    const auto& allocation = *live[i];
    HeapObjectView view;
    view.ptr = allocation.ptr;
    view.size = allocation.size;
    view.owner = allocation.owner;
    view.alloc_ts = allocation.alloc_ts;
    view.age_ns = at > allocation.alloc_ts ? at - allocation.alloc_ts : 0;
    view.alloc_record = allocation.alloc_record;
    view.site = allocation.site;
    view.klass = allocation.klass;
    result.objects.push_back(std::move(view));
  }
  result.objects_truncated = live.size() - result.objects.size();
  return result;
}

std::vector<HeapAllocation> live_allocations(const Trace& trace, uint64_t at,
                                             size_t limit) {
  const Reconstruction recon = reconstruct(trace, at);
  std::vector<HeapAllocation> out;
  out.reserve(recon.live.size());
  for (const auto& allocation : recon.allocations) {
    if (!allocation.freed) out.push_back(allocation);
  }
  std::sort(out.begin(), out.end(),
            [](const HeapAllocation& a, const HeapAllocation& b) {
              if (a.size != b.size) return a.size > b.size;
              return a.ptr < b.ptr;
            });
  if (out.size() > limit) out.resize(limit);
  return out;
}

std::vector<LeakCandidate> leak_candidates(const Trace& trace, size_t limit) {
  limit = std::min<size_t>(limit == 0 ? 1 : limit, 1000);
  const uint64_t end = trace.end_ts();
  const Reconstruction recon = reconstruct(trace, end);
  std::unordered_map<std::string, LeakCandidate> grouped;
  for (const auto& allocation : recon.allocations) {
    if (allocation.freed) continue;
    auto& candidate = grouped[site_key(allocation.site, allocation.klass)];
    candidate.site = allocation.site;
    candidate.klass = allocation.klass;
    candidate.bytes += allocation.size;
    candidate.count += 1;
    if (candidate.oldest_ts == 0 || allocation.alloc_ts < candidate.oldest_ts) {
      candidate.oldest_ts = allocation.alloc_ts;
    }
    candidate.newest_ts = std::max(candidate.newest_ts, allocation.alloc_ts);
  }
  std::vector<LeakCandidate> out;
  out.reserve(grouped.size());
  const double duration = static_cast<double>(end == 0 ? 1 : end);
  for (auto& [key, candidate] : grouped) {
    (void)key;
    const double age_fraction =
        std::clamp(static_cast<double>(end - candidate.oldest_ts) / duration, 0.0,
                   1.0);
    candidate.score = static_cast<double>(candidate.bytes) * (1.0 + 4.0 * age_fraction);
    out.push_back(std::move(candidate));
  }
  std::sort(out.begin(), out.end(),
            [](const LeakCandidate& a, const LeakCandidate& b) {
              if (a.score != b.score) return a.score > b.score;
              if (a.bytes != b.bytes) return a.bytes > b.bytes;
              if (a.site != b.site) return a.site < b.site;
              return a.klass < b.klass;
            });
  if (out.size() > limit) out.resize(limit);
  return out;
}

}  // namespace gbx
