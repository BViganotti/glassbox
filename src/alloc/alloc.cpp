// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/alloc.hpp"

#include "alloc/hook_suppress.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gbx {

std::string SourceLoc::to_string() const {
  if (file.empty() && line == 0) return "<unknown>";
  std::string out(file);
  if (line > 0) {
    out += ":";
    out += std::to_string(line);
  }
  return out;
}

struct ObjectRegistry::Impl {
  explicit Impl(size_t max) : max_objects(max) {}

  mutable std::mutex mu;
  std::unordered_map<uint64_t, Entry> live;
  size_t max_objects = 100000;
  uint64_t overflow = 0;
  uint64_t bytes = 0;
};

ObjectRegistry::ObjectRegistry(size_t max_objects)
    : impl_(new Impl(max_objects == 0 ? 1 : max_objects)) {}

ObjectRegistry::~ObjectRegistry() = default;

bool ObjectRegistry::add(const Entry& entry) {
  if (entry.ptr == 0) return false;
  std::lock_guard lock(impl_->mu);
  auto it = impl_->live.find(entry.ptr);
  if (it != impl_->live.end()) {
    impl_->bytes -= std::min(impl_->bytes, it->second.size);
    impl_->bytes += entry.size;
    it->second = entry;
    return true;
  }
  if (impl_->live.size() >= impl_->max_objects) {
    ++impl_->overflow;
    return false;
  }
  impl_->live.emplace(entry.ptr, entry);
  impl_->bytes += entry.size;
  return true;
}

bool ObjectRegistry::remove(uint64_t ptr) {
  std::lock_guard lock(impl_->mu);
  auto it = impl_->live.find(ptr);
  if (it == impl_->live.end()) return false;
  impl_->bytes -= std::min(impl_->bytes, it->second.size);
  impl_->live.erase(it);
  return true;
}

std::optional<ObjectRegistry::Entry> ObjectRegistry::find(uint64_t ptr) const {
  std::lock_guard lock(impl_->mu);
  auto it = impl_->live.find(ptr);
  if (it == impl_->live.end()) return std::nullopt;
  return it->second;
}

std::vector<ObjectRegistry::Entry> ObjectRegistry::children(
    uint64_t owner) const {
  std::vector<Entry> out;
  std::lock_guard lock(impl_->mu);
  for (const auto& [ptr, entry] : impl_->live) {
    (void)ptr;
    if (entry.owner == owner) out.push_back(entry);
  }
  std::sort(out.begin(), out.end(),
            [](const Entry& a, const Entry& b) { return a.ptr < b.ptr; });
  return out;
}

std::vector<ObjectRegistry::Entry> ObjectRegistry::roots() const {
  std::vector<Entry> out;
  std::lock_guard lock(impl_->mu);
  for (const auto& [ptr, entry] : impl_->live) {
    (void)ptr;
    if (entry.owner == 0) out.push_back(entry);
  }
  std::sort(out.begin(), out.end(),
            [](const Entry& a, const Entry& b) { return a.ptr < b.ptr; });
  return out;
}

std::vector<ObjectRegistry::Entry> ObjectRegistry::entries() const {
  std::vector<Entry> out;
  std::lock_guard lock(impl_->mu);
  out.reserve(impl_->live.size());
  for (const auto& [ptr, entry] : impl_->live) {
    (void)ptr;
    out.push_back(entry);
  }
  std::sort(out.begin(), out.end(),
            [](const Entry& a, const Entry& b) { return a.ptr < b.ptr; });
  return out;
}

size_t ObjectRegistry::size() const {
  std::lock_guard lock(impl_->mu);
  return impl_->live.size();
}

uint64_t ObjectRegistry::overflow_count() const {
  std::lock_guard lock(impl_->mu);
  return impl_->overflow;
}

uint64_t ObjectRegistry::live_bytes() const {
  std::lock_guard lock(impl_->mu);
  return impl_->bytes;
}

void ObjectRegistry::clear() {
  std::lock_guard lock(impl_->mu);
  impl_->live.clear();
  impl_->bytes = 0;
}

// ---------------------------------------------------------------------------

AllocationTracker::AllocationTracker(Recorder& recorder, size_t max_objects)
    : recorder_(recorder), registry_(max_objects) {}

AllocationTracker::~AllocationTracker() = default;

void AllocationTracker::alloc(const void* ptr, size_t size, const SourceLoc& loc,
                              std::string_view klass, const void* owner) {
  detail::AllocHookSuppress suppress;
  if (ptr == nullptr) return;
  const std::string site = loc.to_string();
  recorder_.alloc(ptr, size, site, klass, owner);
  ObjectRegistry::Entry entry;
  entry.ptr = reinterpret_cast<uintptr_t>(ptr);
  entry.size = size;
  entry.owner = reinterpret_cast<uintptr_t>(owner);
  entry.site = site;
  entry.klass = std::string(klass);
  registry_.add(entry);
}

void AllocationTracker::free(const void* ptr) {
  detail::AllocHookSuppress suppress;
  if (ptr == nullptr) return;
  recorder_.free(ptr);
  registry_.remove(reinterpret_cast<uintptr_t>(ptr));
}

bool AllocationTracker::free_tracked(const void* ptr) {
  detail::AllocHookSuppress suppress;
  if (ptr == nullptr) return false;
  const bool known = registry_.remove(reinterpret_cast<uintptr_t>(ptr));
  if (known) recorder_.free(ptr);
  return known;
}

bool AllocationTracker::is_live(const void* ptr) const {
  if (ptr == nullptr) return false;
  return registry_.find(reinterpret_cast<uintptr_t>(ptr)).has_value();
}

std::optional<ObjectRegistry::Entry> AllocationTracker::find(
    const void* ptr) const {
  if (ptr == nullptr) return std::nullopt;
  return registry_.find(reinterpret_cast<uintptr_t>(ptr));
}

AllocationSummary AllocationTracker::summary() const {
  AllocationSummary summary;
  summary.tracked_objects = registry_.size();
  summary.live_objects = summary.tracked_objects;
  summary.live_bytes = registry_.live_bytes();
  summary.untracked_overflows = registry_.overflow_count();
  return summary;
}

std::vector<AllocationSiteSummary> AllocationTracker::sites(size_t limit) const {
  std::unordered_map<std::string, AllocationSiteSummary> grouped;
  for (const auto& entry : registry_.entries()) {
    const std::string key = entry.site + "\x1f" + entry.klass;
    auto& summary = grouped[key];
    summary.site = entry.site;
    summary.klass = entry.klass;
    summary.bytes += entry.size;
    ++summary.count;
  }
  std::vector<AllocationSiteSummary> out;
  out.reserve(grouped.size());
  for (auto& [key, summary] : grouped) {
    (void)key;
    out.push_back(std::move(summary));
  }
  std::sort(out.begin(), out.end(),
            [](const AllocationSiteSummary& a, const AllocationSiteSummary& b) {
              if (a.bytes != b.bytes) return a.bytes > b.bytes;
              if (a.site != b.site) return a.site < b.site;
              return a.klass < b.klass;
            });
  if (out.size() > limit) out.resize(limit);
  return out;
}

}  // namespace gbx
