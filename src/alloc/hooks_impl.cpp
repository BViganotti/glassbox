// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Opt-in replacement for the global allocation functions.
//
// This translation unit is built into the separate `glassbox_hooks` library.
// Linking that library into a host program is the opt-in switch: without it,
// Glassbox never touches the host allocator. Every hook checks a runtime flag
// first, so even when linked, nothing happens until install_allocation_hooks()
// is called.
#include "glassbox/alloc_hooks.hpp"

#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>

#include "alloc/hook_suppress.hpp"

namespace gbx {
namespace {

struct Runtime {
  Runtime(Recorder& recorder, size_t max_objects)
      : tracker(recorder, max_objects) {}

  AllocationTracker tracker;
  uint32_t sample_every = 1;
  std::atomic<uint64_t> counter{0};
};

std::atomic<Runtime*> g_runtime{nullptr};
thread_local bool g_in_hook = false;

/// RAII recursion guard: bookkeeping inside the hook allocates, and those
/// allocations must go straight to malloc/free.
struct HookGuard {
  HookGuard() : previous(g_in_hook) { g_in_hook = true; }
  ~HookGuard() { g_in_hook = previous; }
  bool previous;
};

std::string site_from_address(void* address) {
  Dl_info info{};
  if (address != nullptr && ::dladdr(address, &info) != 0) {
    if (info.dli_sname != nullptr && info.dli_sname[0] != '\0') {
      return std::string(info.dli_sname);
    }
    if (info.dli_fname != nullptr) {
      return std::string(info.dli_fname) + "+" +
             std::to_string(reinterpret_cast<uintptr_t>(address) -
                            reinterpret_cast<uintptr_t>(info.dli_fbase));
    }
  }
  return "<intercepted allocation>";
}

void track_alloc(void* ptr, size_t size, void* return_address) {
  Runtime* runtime = g_runtime.load(std::memory_order_acquire);
  if (runtime == nullptr || g_in_hook || detail::alloc_hooks_suppressed()) return;
  if (runtime->sample_every > 1) {
    const uint64_t n = runtime->counter.fetch_add(1, std::memory_order_relaxed);
    if (n % runtime->sample_every != 0) return;
  }
  try {
    HookGuard guard;
    const std::string site = site_from_address(return_address);
    runtime->tracker.alloc(ptr, size, SourceLoc{site, 0, {}}, "heap");
  } catch (...) {
    // Allocation tracing must never break the host application.
  }
}

void track_free(void* ptr) {
  Runtime* runtime = g_runtime.load(std::memory_order_acquire);
  if (runtime == nullptr || g_in_hook || ptr == nullptr ||
      detail::alloc_hooks_suppressed()) {
    return;
  }
  try {
    HookGuard guard;
    runtime->tracker.free_tracked(ptr);
  } catch (...) {
  }
}

void* allocate(size_t size) {
  void* ptr = std::malloc(size == 0 ? 1 : size);
  if (ptr == nullptr) {
    std::new_handler handler = std::get_new_handler();
    while (handler != nullptr) {
      handler();
      ptr = std::malloc(size == 0 ? 1 : size);
      if (ptr != nullptr) break;
      handler = std::get_new_handler();
    }
    if (ptr == nullptr) throw std::bad_alloc();
  }
  return ptr;
}

void* allocate_aligned(size_t size, size_t alignment) {
  void* ptr = nullptr;
  if (::posix_memalign(&ptr, alignment, size == 0 ? 1 : size) != 0) {
    throw std::bad_alloc();
  }
  return ptr;
}

}  // namespace

Status install_allocation_hooks(const HookConfig& config) {
  if (config.recorder == nullptr) {
    return Status::error(ErrorCode::InvalidArgument,
                         "hook config requires a recorder");
  }
  if (!config.recorder->is_recording()) {
    return Status::error(ErrorCode::InvalidArgument,
                         "start the recorder before installing hooks");
  }
  if (config.sample_every == 0) {
    return Status::error(ErrorCode::InvalidArgument,
                         "sample_every must be at least 1");
  }
  if (g_runtime.load(std::memory_order_acquire) != nullptr) {
    return Status::error(ErrorCode::Busy, "allocation hooks already installed");
  }
  auto* runtime = new Runtime(*config.recorder, config.max_tracked_objects);
  runtime->sample_every = config.sample_every;
  g_runtime.store(runtime, std::memory_order_release);
  return Status::success();
}

void uninstall_allocation_hooks() {
  Runtime* runtime = g_runtime.exchange(nullptr, std::memory_order_acq_rel);
  delete runtime;
}

bool allocation_hooks_installed() {
  return g_runtime.load(std::memory_order_acquire) != nullptr;
}

}  // namespace gbx

// ---------------------------------------------------------------------------
// Global allocation functions. Defined only in glassbox_hooks.
// ---------------------------------------------------------------------------

void* operator new(std::size_t size) {
  void* ptr = gbx::allocate(size);
  gbx::track_alloc(ptr, size, __builtin_return_address(0));
  return ptr;
}

void* operator new[](std::size_t size) {
  void* ptr = gbx::allocate(size);
  gbx::track_alloc(ptr, size, __builtin_return_address(0));
  return ptr;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  const size_t align = static_cast<size_t>(alignment);
  void* ptr = gbx::allocate_aligned(size, align);
  gbx::track_alloc(ptr, size, __builtin_return_address(0));
  return ptr;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* ptr) noexcept {
  if (ptr == nullptr) return;
  gbx::track_free(ptr);
  std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
  ::operator delete(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  ::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  ::operator delete(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  ::operator delete(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  ::operator delete(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  ::operator delete(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  ::operator delete(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  ::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  ::operator delete(ptr);
}
