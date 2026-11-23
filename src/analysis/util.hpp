// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace gbx::analysis {

inline char lower_ascii(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline bool icontains(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool equal = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (lower_ascii(haystack[i + j]) != lower_ascii(needle[j])) {
        equal = false;
        break;
      }
    }
    if (equal) return true;
  }
  return false;
}

/// Locale-independent short rendering used in JSON and diagnostics.
inline std::string format_double(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  return std::string(buffer);
}

}  // namespace gbx::analysis
