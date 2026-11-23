// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "ui/live.hpp"

#include <chrono>
#include <utility>

namespace gbx::ui {

LiveFeed::LiveFeed() = default;

void LiveFeed::reset() {
  {
    std::lock_guard lock(mu_);
    events_.clear();
    next_seq_ = 1;
    dropped_ = 0;
    done_ = false;
    stopped_ = false;
  }
  cv_.notify_all();
}

void LiveFeed::publish(std::string name, std::string data) {
  {
    std::lock_guard lock(mu_);
    if (stopped_ || done_) return;
    if (data.size() > kMaxData) {
      ++dropped_;
      return;
    }
    Event event;
    event.seq = next_seq_++;
    event.name = std::move(name);
    event.data = std::move(data);
    events_.push_back(std::move(event));
    while (events_.size() > kCapacity) {
      events_.pop_front();
      ++dropped_;
    }
  }
  cv_.notify_all();
}

void LiveFeed::finish(std::string data) {
  {
    std::lock_guard lock(mu_);
    if (done_) return;
    if (data.size() <= kMaxData) {
      Event event;
      event.seq = next_seq_++;
      event.name = "done";
      event.data = std::move(data);
      events_.push_back(std::move(event));
      while (events_.size() > kCapacity) {
        events_.pop_front();
        ++dropped_;
      }
    }
    done_ = true;
  }
  cv_.notify_all();
}

void LiveFeed::stop() {
  {
    std::lock_guard lock(mu_);
    stopped_ = true;
  }
  cv_.notify_all();
}

bool LiveFeed::poll(uint64_t& cursor, std::vector<Event>& out, bool& done,
                    int timeout_ms) {
  std::unique_lock lock(mu_);
  out.clear();
  auto ready = [this, &cursor] {
    return stopped_ || done_ || (!events_.empty() && events_.back().seq > cursor);
  };
  if (!ready()) {
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
  }
  done = done_;
  if (stopped_) return false;
  if (events_.empty()) return false;

  const uint64_t oldest = events_.front().seq;
  if (cursor < oldest) {
    Event gap;
    gap.seq = oldest - 1;
    gap.name = "gap";
    gap.data = "{\"dropped\":" + std::to_string(dropped_) + "}";
    out.push_back(std::move(gap));
    cursor = oldest - 1;
  }
  for (const auto& event : events_) {
    if (event.seq <= cursor) continue;
    out.push_back(event);
    cursor = event.seq;
    if (out.size() >= kMaxBatch) break;
  }
  return !out.empty();
}

bool LiveFeed::stopped() const {
  std::lock_guard lock(mu_);
  return stopped_;
}

uint64_t LiveFeed::dropped() const {
  std::lock_guard lock(mu_);
  return dropped_;
}

bool LiveFeed::finished() const {
  std::lock_guard lock(mu_);
  return done_;
}

}  // namespace gbx::ui
