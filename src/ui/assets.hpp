// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <span>
#include <string_view>

namespace gbx::ui::assets {

struct Asset {
  std::string_view name;
  std::string_view data;
};

/// Returns every asset embedded at build time.
std::span<const Asset> all();

/// Finds an asset by exact file name; returns nullptr when absent.
const Asset* find(std::string_view name);

}  // namespace gbx::ui::assets
