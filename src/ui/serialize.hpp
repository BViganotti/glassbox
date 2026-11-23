// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "glassbox/json.hpp"
#include "glassbox/recorder.hpp"
#include "glassbox/trace.hpp"

namespace gbx::ui {

/// Serializes one record as a JSON object. Values keep their native JSON type
/// where possible; integers beyond 2^53 become strings to avoid precision loss.
void write_value(json::Writer& writer, const Trace& trace, const Value& value);

void write_record(json::Writer& writer, const Trace& trace,
                  const Record& record, uint32_t index);

std::string record_json(const Trace& trace, const Record& record,
                        uint32_t index);

/// Serializes a live event resolved by the recorder sink.
void write_live_record(json::Writer& writer, const LiveRecord& record);

std::string live_record_json(const LiveRecord& record);

}  // namespace gbx::ui
