// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gbx::trace::format {

inline constexpr std::string_view kMagic = "GLASSBOX";
inline constexpr std::string_view kChunkMagic = "GBXCHNK1";
inline constexpr std::string_view kFooterMagic = "GBXEND01";
inline constexpr uint32_t kVersion = 1;

// magic(8) + version(4) + flags(4) + wall_ns(8) + pid(4) + meta_count(4)
inline constexpr size_t kFileHeaderFixed = 32;
// magic(8) + payload_len(4) + crc(4) + record_count(4)
inline constexpr size_t kChunkHeaderSize = 20;
// magic(8) + chunk_count(8) + record_count(8) + duration(8) + crc(4)
inline constexpr size_t kFooterSize = 36;

inline constexpr uint32_t kMaxChunkPayload = 8u << 20;
inline constexpr uint32_t kMaxChunkRecords = 1'000'000;
inline constexpr uint32_t kMaxChunkStrings = 65536;
inline constexpr uint32_t kMaxMetaStrings = 16;

}  // namespace gbx::trace::format
