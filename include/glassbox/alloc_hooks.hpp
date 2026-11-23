// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstddef>

#include "glassbox/alloc.hpp"
#include "glassbox/recorder.hpp"
#include "glassbox/status.hpp"

namespace gbx {

/// Opt-in interception of the host program's global operator new/delete.
///
/// This is a sharp tool. It only has an effect in programs that link the
/// separate `glassbox_hooks` static library, which replaces the global
/// allocation functions. Calls are attributed via `dladdr` (symbol name plus
/// module offset); no source line information is available for intercepted
/// allocations.
///
/// Rules:
///  - Install after starting `recorder`, uninstall before finishing it.
///  - Call install/uninstall when no other thread is allocating.
///  - `sample_every` > 1 records one allocation out of N; frees are only
///    recorded for tracked allocations, so the heap view is a sample.
///  - Not supported together with sanitizers (they own the allocator).
struct HookConfig {
  Recorder* recorder = nullptr;
  uint32_t sample_every = 1;
  size_t max_tracked_objects = 200000;
};

/// Installs the hooks. Returns LimitExceeded/Busy style errors on misuse.
Status install_allocation_hooks(const HookConfig& config);

/// Removes the hooks. Idempotent.
void uninstall_allocation_hooks();

bool allocation_hooks_installed();

}  // namespace gbx
