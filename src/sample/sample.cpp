// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/sample.hpp"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>

#if defined(__aarch64__)
#include <mach/arm/thread_status.h>
#elif defined(__x86_64__)
#include <mach/i386/thread_status.h>
#endif

#include <cxxabi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "alloc/hook_suppress.hpp"

namespace gbx {
namespace {

std::string demangle(std::string_view name) {
  if (name.empty()) return {};
  if (name.size() > 4096) return std::string(name.substr(0, 4096));
  int status = 0;
  char* demangled =
      abi::__cxa_demangle(std::string(name).c_str(), nullptr, nullptr, &status);
  if (status == 0 && demangled != nullptr) {
    std::string out(demangled);
    std::free(demangled);
    return out;
  }
  std::free(demangled);
  return std::string(name);
}

/// Reads memory from our own task without risking a fault on a bad pointer.
bool safe_read(uintptr_t address, void* out, size_t size) {
  if (address == 0 || size == 0) return false;
  mach_vm_size_t read = 0;
  const kern_return_t kr = mach_vm_read_overwrite(
      mach_task_self(), static_cast<mach_vm_address_t>(address),
      static_cast<mach_vm_size_t>(size),
      reinterpret_cast<mach_vm_address_t>(out), &read);
  return kr == KERN_SUCCESS && read == size;
}

constexpr size_t kMaxStackSpan = 1u << 20;  // 1 MiB of stack per walk
constexpr uintptr_t kMinStackPointer = 0x1000;

/// Walks a frame-pointer chain, returning program counters leaf-first.
/// Invalid or non-monotonic frames end the walk; nothing is dereferenced
/// directly, so a corrupt frame cannot crash the process.
size_t walk_frame_pointers(uintptr_t pc, uintptr_t fp,
                           std::vector<uintptr_t>& out, size_t max_frames) {
  out.clear();
  if (pc != 0) out.push_back(pc);
  uintptr_t current = fp;
  uintptr_t previous = 0;
  while (out.size() < max_frames && current != 0) {
    if (current % 8 != 0 || current < kMinStackPointer) break;
    if (previous != 0) {
      if (current <= previous || current - previous > kMaxStackSpan) break;
    }
    uintptr_t frame[2] = {0, 0};
    if (!safe_read(current, frame, sizeof(frame))) break;
    const uintptr_t next = frame[0];
    const uintptr_t return_address = frame[1];
    if (next == 0 || return_address == 0) break;
    if (next % 8 != 0 || next <= current || next - current > kMaxStackSpan) {
      break;
    }
    if (out.empty() || return_address != out.back()) {
      out.push_back(return_address);
    }
    previous = current;
    current = next;
  }
  return out.size();
}

}  // namespace

SymbolInfo symbolize_address(uintptr_t address) {
  SymbolInfo info;
  info.address = address;
  Dl_info dl{};
  if (address != 0 && ::dladdr(reinterpret_cast<void*>(address), &dl) != 0) {
    if (dl.dli_fname != nullptr) info.module = dl.dli_fname;
    if (dl.dli_fbase != nullptr) {
      info.offset = address - reinterpret_cast<uintptr_t>(dl.dli_fbase);
    }
    if (dl.dli_sname != nullptr) info.name = demangle(dl.dli_sname);
  }
  if (info.name.empty()) {
    if (!info.module.empty()) {
      info.name = info.module + "+" + std::to_string(info.offset);
    } else {
      info.name = "<unknown>";
    }
  }
  return info;
}

SymbolInfo symbolize_pointer(const void* pointer) {
  return symbolize_address(reinterpret_cast<uintptr_t>(pointer));
}

std::string short_symbol(std::string_view name, size_t limit) {
  std::string out;
  const size_t paren = name.find('(');
  std::string_view trimmed = paren == std::string_view::npos
                                 ? name
                                 : name.substr(0, paren);
  if (trimmed.empty()) trimmed = name;
  for (char c : trimmed) {
    const unsigned char uc = static_cast<unsigned char>(c);
    out.push_back(uc < 0x20 || uc == 0x7f ? '?' : c);
  }
  if (out.size() > limit) {
    out.resize(limit > 3 ? limit - 3 : limit);
    out += "...";
  }
  return out;
}

void ManualSampler::capture(std::string_view phase, size_t max_frames) {
  max_frames = std::clamp<size_t>(max_frames, 1, 64);
  const uintptr_t pc =
      reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  const uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  std::vector<uintptr_t> frames;
  walk_frame_pointers(pc, fp, frames, max_frames - (phase.empty() ? 0 : 1));
  std::vector<std::string> names;
  names.reserve(frames.size() + 1);
  if (!phase.empty()) names.emplace_back(short_symbol(phase));
  for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
    names.push_back(short_symbol(symbolize_address(*it).name));
  }
  if (names.empty()) return;
  std::vector<std::string_view> views;
  views.reserve(names.size());
  for (const auto& name : names) views.push_back(name);
  recorder_.sample(views, 1.0);
}

// ---------------------------------------------------------------------------

struct MachSampler::Impl {
  MachSamplerConfig config;
  std::thread thread;
  std::mutex mu;
  std::condition_variable cv;
  bool stop_requested = false;
  bool running = false;
  std::atomic<uint64_t> samples{0};
  std::atomic<uint64_t> thread_states{0};
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> failed_states{0};
  std::atomic<uint64_t> errors{0};

  void loop() {
    detail::AllocHookSuppress suppress;
    pthread_setname_np("gbx-sampler");
    std::unique_lock lock(mu);
    while (!stop_requested) {
      lock.unlock();
      take_sample();
      lock.lock();
      cv.wait_for(lock, std::chrono::microseconds(config.interval_us),
                  [this] { return stop_requested; });
    }
  }

  void take_sample() {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
      ++errors;
      return;
    }
    const mach_msg_type_number_t limit =
        static_cast<mach_msg_type_number_t>(std::min<size_t>(
            count, std::min<uint32_t>(config.max_threads, 256)));
    bool recorded = false;
    std::vector<uintptr_t> frame_pcs;
    for (mach_msg_type_number_t i = 0; i < limit; ++i) {
      thread_t thread_port = threads[i];
      if (thread_port == MACH_PORT_NULL) continue;
#if defined(__aarch64__)
      arm_thread_state64_t state{};
      mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
      const kern_return_t kr = thread_get_state(
          thread_port, ARM_THREAD_STATE64,
          reinterpret_cast<thread_state_t>(&state),
          &state_count);
      const uintptr_t pc = kr == KERN_SUCCESS ? state.__pc : 0;
      const uintptr_t fp = kr == KERN_SUCCESS ? state.__fp : 0;
#elif defined(__x86_64__)
      x86_thread_state64_t state{};
      mach_msg_type_number_t state_count = x86_THREAD_STATE64_COUNT;
      const kern_return_t kr = thread_get_state(
          thread_port, x86_THREAD_STATE64,
          reinterpret_cast<thread_state_t>(&state),
          &state_count);
      const uintptr_t pc = kr == KERN_SUCCESS ? state.rip : 0;
      const uintptr_t fp = kr == KERN_SUCCESS ? state.rbp : 0;
#else
      const kern_return_t kr = KERN_FAILURE;
      const uintptr_t pc = 0;
      const uintptr_t fp = 0;
#endif
      if (kr != KERN_SUCCESS || pc == 0) {
        ++failed_states;
        mach_port_deallocate(mach_task_self(), thread_port);
        continue;
      }
      ++thread_states;
      frame_pcs.clear();
      if (config.capture_frame_pointers) {
        walk_frame_pointers(pc, fp, frame_pcs, config.max_frames);
      } else {
        frame_pcs.push_back(pc);
      }
      if (frame_pcs.empty()) {
        mach_port_deallocate(mach_task_self(), thread_port);
        continue;
      }
      std::vector<std::string> names;
      names.reserve(frame_pcs.size());
      for (auto it = frame_pcs.rbegin(); it != frame_pcs.rend(); ++it) {
        names.push_back(short_symbol(symbolize_address(*it).name));
      }
      std::vector<std::string_view> views;
      views.reserve(names.size());
      for (const auto& name : names) views.push_back(name);
      config.recorder->sample(views, 1.0);
      frames += views.size();
      recorded = true;
      mach_port_deallocate(mach_task_self(), thread_port);
    }
    if (threads != nullptr) {
      vm_deallocate(mach_task_self(),
                    reinterpret_cast<vm_address_t>(threads),
                    count * sizeof(thread_t));
    }
    if (recorded) ++samples;
  }
};

MachSampler::MachSampler() = default;

MachSampler::~MachSampler() { stop(); }

Status MachSampler::start(const MachSamplerConfig& config) {
  if (impl_ && impl_->running) {
    return Status::error(ErrorCode::Busy, "sampler already running");
  }
  if (config.recorder == nullptr) {
    return Status::error(ErrorCode::InvalidArgument,
                         "sampler requires a recorder");
  }
  if (!config.recorder->is_recording()) {
    return Status::error(ErrorCode::InvalidArgument,
                         "start the recorder before the sampler");
  }
  impl_ = std::make_unique<Impl>();
  impl_->config = config;
  impl_->config.interval_us =
      std::clamp<uint32_t>(config.interval_us, 100, 1'000'000);
  impl_->config.max_frames = std::clamp<size_t>(config.max_frames, 1, 64);
  impl_->config.max_threads = std::clamp<uint32_t>(config.max_threads, 1, 256);
  impl_->running = true;
  impl_->thread = std::thread([ptr = impl_.get()] { ptr->loop(); });
  return Status::success();
}

void MachSampler::stop() {
  if (!impl_ || !impl_->running) return;
  {
    std::lock_guard lock(impl_->mu);
    impl_->stop_requested = true;
  }
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->running = false;
}

bool MachSampler::running() const { return impl_ && impl_->running; }

MachSamplerStats MachSampler::stats() const {
  MachSamplerStats out;
  if (!impl_) return out;
  out.samples = impl_->samples.load();
  out.thread_states = impl_->thread_states.load();
  out.frames = impl_->frames.load();
  out.failed_states = impl_->failed_states.load();
  out.errors = impl_->errors.load();
  return out;
}

}  // namespace gbx
