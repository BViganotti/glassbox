// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Thread-local suppression bridge between the core library and the optional
// allocation hooks. Glassbox's own bookkeeping allocations must never be
// recorded (that would create a feedback loop in the writer thread), so every
// recorder entry point suppresses hooks while it runs.
#pragma once

namespace gbx::detail {

extern thread_local int g_alloc_hook_suppression_depth;

inline bool alloc_hooks_suppressed() {
  return g_alloc_hook_suppression_depth > 0;
}

/// RAII guard that suspends allocation interception on the current thread.
class AllocHookSuppress {
 public:
  AllocHookSuppress() { ++g_alloc_hook_suppression_depth; }
  ~AllocHookSuppress() { --g_alloc_hook_suppression_depth; }
  AllocHookSuppress(const AllocHookSuppress&) = delete;
  AllocHookSuppress& operator=(const AllocHookSuppress&) = delete;
};

}  // namespace gbx::detail
