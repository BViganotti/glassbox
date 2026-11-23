// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/demo.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "glassbox/alloc.hpp"
#include "glassbox/analysis.hpp"
#include "glassbox/sample.hpp"
#include "glassbox/span.hpp"

namespace gbx {
namespace {

struct Rng {
  uint32_t state;

  explicit Rng(uint32_t seed) : state(seed == 0 ? 1 : seed) {}

  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }

  double unit() { return static_cast<double>(next() & 0xffffff) / 16777215.0; }
};

struct DemoNode {
  uint64_t id = 0;
  std::vector<DemoNode*> children;
};

__attribute__((noinline)) void simulate_physics(ManualSampler& sampler,
                                                double& energy) {
  sampler.capture("physics");
  energy += 0.25;
  __asm__ __volatile__("" ::: "memory");
}

__attribute__((noinline)) void simulate_animation(ManualSampler& sampler,
                                                  double& energy) {
  sampler.capture("animation");
  energy += 0.5;
  __asm__ __volatile__("" ::: "memory");
}

__attribute__((noinline)) void simulate_render(ManualSampler& sampler,
                                               uint32_t& draw_calls) {
  sampler.capture("render");
  draw_calls += 3;
  __asm__ __volatile__("" ::: "memory");
}

__attribute__((noinline)) void simulate_slow_frame(ManualSampler& sampler,
                                                   bool sleep) {
  sampler.capture("slow-frame");
  if (sleep) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  } else {
    volatile uint64_t accumulator = 0;
    for (int i = 0; i < 20000; ++i) {
      accumulator += static_cast<uint64_t>(i) * 2654435761u;
    }
    (void)accumulator;
  }
}

}  // namespace

Status run_demo(Recorder& recorder, const DemoOptions& options,
                DemoResult& result) {
  result = DemoResult{};
  if (!recorder.is_recording()) {
    result.status = Status::error(ErrorCode::InvalidArgument,
                                  "demo requires a started recorder");
    return result.status;
  }
  Rng rng(options.seed);
  recorder.thread_name("sim-main");
  recorder.log(LogLevel::Info, "demo workload starting", "boot");
  recorder.counter("boot_progress", 1.0);

  AllocationTracker tracker(recorder, 200000);
  ManualSampler sampler(recorder);
  MachSampler mach;
  if (options.use_mach_sampler) {
    MachSamplerConfig config;
    config.recorder = &recorder;
    config.interval_us = 3000;
    config.max_frames = 16;
    const auto status = mach.start(config);
    if (!status.ok()) {
      recorder.log(LogLevel::Warn,
                   "mach sampler unavailable: " + status.message(),
                   "sampling");
    }
  }

  // A live scene graph gives the heap view an ownership hierarchy with
  // meaningful retained sizes. It is intentionally still alive when the
  // recording ends.
  auto* scene = new DemoNode();
  scene->id = 1;
  tracker.alloc(scene, sizeof(DemoNode), {"scene.cpp", 41, "build_scene"},
                "Scene", nullptr);
  for (uint32_t i = 0; i < 6; ++i) {
    auto* child = new DemoNode();
    child->id = i + 2;
    scene->children.push_back(child);
    tracker.alloc(child, sizeof(DemoNode), {"scene.cpp", 47, "build_scene"},
                  "SceneNode", scene);
  }

  std::vector<std::vector<uint8_t>*> leaked_textures;
  std::vector<std::pair<std::vector<uint8_t>*, uint64_t>> pending_frees;
  std::vector<std::vector<uint8_t>*> entities;
  uint64_t allocations = 0;
  uint64_t samples = 0;
  uint32_t draw_calls = 0;
  double energy = 0;

  const uint32_t frames = std::min<uint32_t>(options.frames, 100000);
  const uint32_t spike_frame = frames / 2;
  for (uint32_t frame_no = 0; frame_no < frames; ++frame_no) {
    auto frame_span = recorder.begin_span("Frame", "cpu");

    for (int i = 0; i < 2; ++i) {
      const size_t size = 512 + (rng.next() % 4096);
      auto* buffer = new std::vector<uint8_t>(size);
      tracker.alloc(buffer, size, {"renderer.cpp", 142, "upload_batch"},
                    "VertexBuffer");
      ++allocations;
      pending_frees.emplace_back(buffer, frame_no);
    }

    {
      auto update = recorder.begin_span("Update", "cpu");
      auto physics = recorder.begin_span("Physics", "cpu");
      if (frame_no % 5 == 0) {
        simulate_physics(sampler, energy);
        ++samples;
      }
      physics.end();
      auto animation = recorder.begin_span("Animation", "cpu");
      if (frame_no % 5 == 0) {
        simulate_animation(sampler, energy);
        ++samples;
      }
      animation.end();
      update.end();
    }

    const bool spike = frame_no == spike_frame;
    if (spike) {
      auto slow = recorder.begin_span("SlowFrame", "cpu");
      recorder.log(LogLevel::Warn, "frame budget exceeded", "performance");
      simulate_slow_frame(sampler, options.sleep_spike);
      ++samples;
      slow.end();
    }

    {
      auto render = recorder.begin_span("Render", "cpu");
      if (frame_no % 3 == 0) {
        simulate_render(sampler, draw_calls);
        ++samples;
      }
      auto culling = recorder.begin_span("Culling", "cpu");
      culling.end();
      auto draw = recorder.begin_span("Draw", "cpu");
      draw.end();
      render.end();
    }

    if (frame_no % 10 == 0) {
      auto* cache = new std::vector<uint8_t>(16384);
      tracker.alloc(cache, 16384, {"texture_cache.cpp", 77, "insert"},
                    "CachedTexture");
      ++allocations;
      pending_frees.emplace_back(cache, frame_no + 3);
    }
    for (auto it = pending_frees.begin(); it != pending_frees.end();) {
      if (it->second <= frame_no) {
        tracker.free(it->first);
        delete it->first;
        it = pending_frees.erase(it);
      } else {
        ++it;
      }
    }

    // Deliberate leaks: the asset loader never releases these textures. They
    // are owned by the scene in the trace, so retained size includes them.
    if (frame_no == 15 || frame_no == 45 || frame_no == 75) {
      auto* texture = new std::vector<uint8_t>(32768);
      tracker.alloc(texture, 32768, {"asset_loader.cpp", 88, "load_texture"},
                    "Texture", scene);
      leaked_textures.push_back(texture);
      ++allocations;
      recorder.log(LogLevel::Error, "texture uploaded without release",
                   "asset_loader");
    }

    if (frame_no % 10 == 0) {
      auto* entity = new std::vector<uint8_t>(256);
      tracker.alloc(entity, 256, {"entities.cpp", 18, "spawn"}, "Entity",
                    scene);
      entities.push_back(entity);
      ++allocations;
      if (entities.size() > 8) {
        tracker.free(entities.front());
        delete entities.front();
        entities.erase(entities.begin());
      }
    }

    recorder.counter("fps", spike ? 14.5 : 59.0 + rng.unit(),
                     spike ? "" : "hz");
    recorder.counter("draw_calls", static_cast<double>(draw_calls));
    recorder.counter("entities", static_cast<double>(frame_no / 10 + 1));
    recorder.gauge("heap_bytes",
                   static_cast<double>(tracker.summary().live_bytes));

    if (frame_no % 5 == 0) {
      recorder.snapshot({{"player_x", std::sin(frame_no * 0.1) * 100.0},
                         {"player_y", std::cos(frame_no * 0.1) * 100.0},
                         {"health", 100.0 - static_cast<double>(frame_no) * 0.1},
                         {"inventory_count",
                          static_cast<double>(3 + frame_no / 5)},
                         {"inventory_expected", 3.0}});
      recorder.snapshot_text(
          {{"mode", spike ? "combat"
                          : (frame_no > frames / 2 ? "explore" : "idle")}});
    }

    if (frame_no % 20 == 0) {
      recorder.log(LogLevel::Info,
                   "frame " + std::to_string(frame_no) + " checkpoint", "demo");
    }
    if (frame_no % 5 == 0) {
      recorder.log(LogLevel::Debug,
                   "inventory diverged: " +
                       std::to_string(3 + frame_no / 5) + " vs 3",
                   "state");
    }
    frame_span.end();
    if (options.frame_delay_us > 0) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(options.frame_delay_us));
    }
  }

  for (auto& [ptr, free_at] : pending_frees) {
    (void)free_at;
    tracker.free(ptr);
    delete ptr;
  }
  pending_frees.clear();

  recorder.log(LogLevel::Info, "demo workload complete", "demo");
  recorder.mark("demo-end");
  if (options.use_mach_sampler) mach.stop();

  result.allocations = allocations;
  result.frames_recorded = frames;
  result.samples = samples;
  result.leaks = static_cast<uint64_t>(tracker.summary().live_objects);
  result.status = Status::success();

  // Free the live objects now that every record has been emitted. Because no
  // Free records follow, the finished trace still shows them alive at the end:
  // that is the intentional leak the demo is about, and the process itself
  // stays clean.
  for (auto* texture : leaked_textures) delete texture;
  for (auto* entity : entities) delete entity;
  for (auto* child : scene->children) delete child;
  delete scene;
  return result.status;
}

DemoResult record_demo_trace(const std::string& path,
                             const DemoOptions& options) {
  DemoResult result;
  const auto started = std::chrono::steady_clock::now();
  Recorder recorder;
  auto status = recorder.start(path);
  if (!status.ok()) {
    result.status = status;
    return result;
  }
  status = run_demo(recorder, options, result);
  if (!status.ok()) {
    recorder.cancel();
    result.status = status;
    return result;
  }
  result.status = recorder.finish();
  result.path = path;
  result.duration_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  return result;
}

}  // namespace gbx
