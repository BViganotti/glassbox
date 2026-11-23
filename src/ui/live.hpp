// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace gbx::ui {

/// Bounded event bus bridging the recorder sink and SSE subscribers. Slow
/// clients skip events (reported via a gap event) instead of growing memory.
class LiveFeed {
 public:
  struct Event {
    uint64_t seq = 0;
    std::string name;
    std::string data;
  };

  LiveFeed();

  void reset();
  void publish(std::string name, std::string data);
  void finish(std::string data);
  void stop();

  /// Waits up to `timeout_ms` for events newer than `cursor`. Returns true
  /// when at least one event (or a gap notice) was produced.
  bool poll(uint64_t& cursor, std::vector<Event>& out, bool& done,
            int timeout_ms);

  bool stopped() const;
  uint64_t dropped() const;
  bool finished() const;

 private:
  static constexpr size_t kCapacity = 1024;
  static constexpr size_t kMaxData = 16 * 1024;
  static constexpr size_t kMaxBatch = 256;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Event> events_;
  uint64_t next_seq_ = 1;
  uint64_t dropped_ = 0;
  bool done_ = false;
  bool stopped_ = false;
};

}  // namespace gbx::ui
