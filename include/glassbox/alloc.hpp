// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "glassbox/recorder.hpp"

namespace gbx {

/// A source location captured at an allocation call site.
struct SourceLoc {
  std::string_view file;
  int line = 0;
  std::string_view function;

  /// Renders "file:line" or just the file when no line is known.
  std::string to_string() const;
};

/// Captures the current source location. Use `GBX_HERE` at call sites.
#define GBX_HERE \
  ::gbx::SourceLoc { __FILE__, __LINE__, __PRETTY_FUNCTION__ }

struct AllocationSummary {
  uint64_t live_objects = 0;
  uint64_t live_bytes = 0;
  uint64_t tracked_objects = 0;
  uint64_t untracked_overflows = 0;
};

struct AllocationSiteSummary {
  std::string site;
  std::string klass;
  uint64_t bytes = 0;
  uint64_t count = 0;
};

/// Bounded thread-safe registry of live objects, used for owner inference and
/// in-process leak inspection. The trace itself remains the source of truth;
/// this registry may stop tracking new objects after `max_objects` to keep
/// memory bounded, and reports that in `overflow_count()`.
class ObjectRegistry {
 public:
  struct Entry {
    uint64_t ptr = 0;
    uint64_t size = 0;
    uint64_t owner = 0;
    uint64_t alloc_ts = 0;
    std::string site;
    std::string klass;
  };

  explicit ObjectRegistry(size_t max_objects = 100000);
  ~ObjectRegistry();

  bool add(const Entry& entry);
  bool remove(uint64_t ptr);
  std::optional<Entry> find(uint64_t ptr) const;
  std::vector<Entry> children(uint64_t owner) const;
  std::vector<Entry> roots() const;
  std::vector<Entry> entries() const;
  size_t size() const;
  uint64_t overflow_count() const;
  uint64_t live_bytes() const;
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Recording-side allocation tracker. Emits Alloc/Free records and maintains
/// a bounded object registry so callers can attribute ownership. All methods
/// are thread-safe.
///
/// Lifetime: the Recorder must outlive the tracker. `alloc` may be called with
/// pointers the tracker has never seen; `free` always emits a Free record and
/// removes any registry entry.
class AllocationTracker {
 public:
  explicit AllocationTracker(Recorder& recorder, size_t max_objects = 100000);
  ~AllocationTracker();
  AllocationTracker(const AllocationTracker&) = delete;
  AllocationTracker& operator=(const AllocationTracker&) = delete;

  void alloc(const void* ptr, size_t size, const SourceLoc& loc,
             std::string_view klass = {}, const void* owner = nullptr);
  void free(const void* ptr);

  /// Used by interception hooks: ignores frees for allocations that were never
  /// recorded (for example because of sampling).
  bool free_tracked(const void* ptr);

  bool is_live(const void* ptr) const;
  std::optional<ObjectRegistry::Entry> find(const void* ptr) const;
  AllocationSummary summary() const;
  std::vector<AllocationSiteSummary> sites(size_t limit = 64) const;
  ObjectRegistry& registry() { return registry_; }
  const ObjectRegistry& registry() const { return registry_; }

 private:
  Recorder& recorder_;
  ObjectRegistry registry_;
};

}  // namespace gbx
