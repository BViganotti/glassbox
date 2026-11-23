// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "glassbox/recorder.hpp"

namespace gbx {

/// RAII handle for an open span. Move-only; ends the span when destroyed.
/// Lifetime: must not outlive its Recorder. Ending twice is a no-op.
class Span {
 public:
  Span() = default;
  Span(Recorder& recorder, uint32_t id) : recorder_(&recorder), id_(id) {}
  ~Span() { end(); }

  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

  Span(Span&& other) noexcept
      : recorder_(other.recorder_), id_(other.id_) {
    other.recorder_ = nullptr;
    other.id_ = 0;
  }

  Span& operator=(Span&& other) noexcept {
    if (this != &other) {
      end();
      recorder_ = other.recorder_;
      id_ = other.id_;
      other.recorder_ = nullptr;
      other.id_ = 0;
    }
    return *this;
  }

  void end() {
    if (recorder_ != nullptr && id_ != 0) {
      recorder_->span_end(id_);
      recorder_ = nullptr;
      id_ = 0;
    }
  }

  bool active() const { return recorder_ != nullptr && id_ != 0; }
  uint32_t id() const { return id_; }

  /// Emits a log line attached to this span.
  void log(LogLevel level, std::string_view message,
           std::string_view category = {}) {
    if (recorder_ != nullptr) recorder_->log(level, message, category);
  }

  /// Emits a counter attached to this span.
  void counter(std::string_view name, double value,
               std::string_view unit = {}) {
    if (recorder_ != nullptr) recorder_->counter(name, value, unit);
  }

 private:
  Recorder* recorder_ = nullptr;
  uint32_t id_ = 0;
};

}  // namespace gbx
