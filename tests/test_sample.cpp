// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/sample.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "glassbox/trace_io.hpp"
#include "test_framework.hpp"

namespace {

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-sample-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  std::filesystem::remove(dir / name);
  return (dir / name).string();
}

}  // namespace

extern "C" __attribute__((noinline)) void gbx_symbol_probe() {
  __asm__ __volatile__("" ::: "memory");
}

namespace {

__attribute__((noinline)) void manual_capture_probe(gbx::ManualSampler& sampler) {
  sampler.capture("probe-phase");
  __asm__ __volatile__("" ::: "memory");
}

__attribute__((noinline)) void busy_work(int iterations) {
  volatile uint64_t accumulator = 0;
  for (int i = 0; i < iterations; ++i) {
    accumulator = accumulator * 6364136223846793005ull +
                  static_cast<uint64_t>(i);
  }
  (void)accumulator;
}

size_t count_samples(const gbx::Trace& trace,
                     std::vector<std::string>* frames_out = nullptr) {
  size_t samples = 0;
  for (const auto& record : trace.records) {
    if (record.type != gbx::RecordType::Sample) continue;
    ++samples;
    const auto frames = trace.str_field(record, "frames");
    CHECK(!frames.empty());
    if (frames_out) frames_out->push_back(std::string(frames));
  }
  return samples;
}

}  // namespace

TEST(symbolize_known_function) {
  const auto info =
      gbx::symbolize_address(reinterpret_cast<uintptr_t>(&gbx_symbol_probe));
  CHECK(!info.name.empty());
  CHECK(info.name.find("gbx_symbol_probe") != std::string::npos ||
        info.name.find("symbol_probe") != std::string::npos);
  CHECK_EQ(info.address, reinterpret_cast<uintptr_t>(&gbx_symbol_probe));
}

TEST(symbolize_unknown_address) {
  const auto info = gbx::symbolize_address(0);
  CHECK_EQ(info.name, std::string("<unknown>"));
  gbx::ManualSampler* null_sampler = nullptr;
  (void)null_sampler;  // keep the "unknown" path independent of sampling
}

TEST(short_symbol_cleans_names) {
  CHECK_EQ(gbx::short_symbol("foo::bar(int, char)"), std::string("foo::bar"));
  CHECK_EQ(gbx::short_symbol("plain"), std::string("plain"));
  CHECK_EQ(gbx::short_symbol(std::string("a\nb")), std::string("a?b"));
  const std::string long_name(400, 'x');
  const auto shortened = gbx::short_symbol(long_name, 40);
  CHECK_EQ(shortened.size(), size_t{40});
  CHECK_EQ(shortened.substr(shortened.size() - 3), std::string("..."));
}

TEST(manual_sampler_records_real_frames) {
  const std::string path = temp_path("manual.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  gbx::ManualSampler sampler(rec);
  manual_capture_probe(sampler);
  manual_capture_probe(sampler);
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  std::vector<std::string> frames;
  const size_t samples = count_samples(trace, &frames);
  CHECK_EQ(samples, size_t{2});
  bool saw_phase = false;
  bool saw_probe = false;
  for (const auto& frame_list : frames) {
    if (frame_list.rfind("probe-phase", 0) == 0) saw_phase = true;
    if (frame_list.find("manual_capture_probe") != std::string::npos) {
      saw_probe = true;
    }
  }
  CHECK(saw_phase);
  CHECK(saw_probe);
}

TEST(mach_sampler_captures_running_process) {
  const std::string path = temp_path("mach.gbt");
  gbx::Recorder rec;
  CHECK(rec.start(path).ok());
  gbx::MachSampler sampler;
  gbx::MachSamplerConfig config;
  config.recorder = &rec;
  config.interval_us = 1000;
  config.max_threads = 8;
  auto status = sampler.start(config);
  CHECK_MSG(status.ok(), status.describe());
  if (status.ok()) {
    CHECK(sampler.running());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
      busy_work(50000);
    }
    sampler.stop();
    CHECK(!sampler.running());
    const auto stats = sampler.stats();
    CHECK_GT(stats.samples, uint64_t{0});
    CHECK_GT(stats.thread_states, uint64_t{0});
    CHECK_GT(stats.frames, uint64_t{0});
  }
  CHECK(rec.finish().ok());
  gbx::Trace trace;
  CHECK(gbx::load_trace(path, trace).ok());
  CHECK_GT(count_samples(trace), size_t{0});
}

TEST(mach_sampler_requires_recording) {
  gbx::MachSampler sampler;
  gbx::MachSamplerConfig config;
  CHECK(!sampler.start(config).ok());  // null recorder
  gbx::Recorder rec;
  config.recorder = &rec;
  CHECK(!sampler.start(config).ok());  // recorder not started
  sampler.stop();
}
