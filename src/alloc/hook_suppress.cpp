// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "alloc/hook_suppress.hpp"

namespace gbx::detail {

thread_local int g_alloc_hook_suppression_depth = 0;

}  // namespace gbx::detail
