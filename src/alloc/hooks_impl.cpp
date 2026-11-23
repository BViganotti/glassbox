// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
// Opt-in global allocation interception lives here. Linking this static
// library replaces operator new/delete for the whole host program, which is
// why it is never part of the main glassbox binary.
namespace gbx::alloc {
namespace {
[[maybe_unused]] int hooks_placeholder = 0;
}
}  // namespace gbx::alloc
