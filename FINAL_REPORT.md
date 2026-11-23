# Glassbox — Final Report

Version 0.1.0 · 2026-11-23 · macOS arm64

Glassbox is a local time-travel inspector for native C++ applications. It records
CPU samples, allocations, spans, logs, counters and state snapshots into a
portable trace, then serves a browser workbench on loopback for scrubbing,
heap-at-time inspection, leak analysis, flame graphs, querying and A/B diffs.
The implementation is dependency-free C++20 with an embedded, original UI.

Repository: https://github.com/BViganotti/glassbox (public, MIT)

## Definition of Done — evidence

| # | Requirement | Status | Evidence |
|---:|---|---|---|
| 1 | Release configure succeeds | ✅ | `cmake -S . -B build-final -G Ninja -DCMAKE_BUILD_TYPE=Release` exited 0 |
| 2 | Build succeeds with zero warnings | ✅ | Clean `build-final` build; `grep -c 'warning:'` → `0`; all targets use `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror` |
| 3 | `ctest` passes | ✅ | 7/7 tests passed in 12.83 s; unit binary: 153 tests / 12,342 checks / 0 failures; hooks: 4 tests / 33 checks; five fuzz modes |
| 4 | ASan/UBSan and TSan pass and are documented | ✅ | ASan+UBSan: 6/6 tests (unit 5.28 s, total 15.10 s), TSan: 6/6 tests (unit 6.64 s, total 18.00 s). Hooks excluded from sanitizer builds by design (sanitizers own the allocator) |
| 5 | Launcher opens the local UI; demo scenario works | ✅ | Headless Chrome rendered overview/flame/heap from the launcher URL with token; browser spawn test intercepts macOS `open` and verifies the exact tokenized URL; golden demo trace and generated demo assertions pass |
| 6 | README, docs, CHANGELOG, LICENSE, LIMITATIONS, FINAL_REPORT exist | ✅ | All present, plus `docs/` (format, recorder, analysis, query, workbench, architecture, security, testing, benchmarks, demo walkthrough, API map) |
| 7 | FINAL_REPORT lists LOC, commits, tests, benchmarks and gaps | ✅ | This document |
| 8 | Repo committed and pushed | ✅ | `origin` = `github.com/BViganotti/glassbox`, branch `main`; all release commits pushed |
| 9 | No external dependency, no CI, no CLI-only product | ✅ | No third-party libraries, no vendored code, no network at runtime; `.github/` absent; product UI is the loopback workbench (examples are ordinary programs) |

## Build and verification environment

| | |
|---|---|
| OS | macOS 26.7.3 (Darwin 25.6.0), arm64 (Apple Silicon) |
| Compiler | Apple Clang 21.0.0 (clang-2100.1.1.101) |
| Build | CMake 4.2.3, Ninja 1.13.0, `-O3 -DNDEBUG`, libc++ hardening |
| CPU | 10 logical cores |
| Trace storage | system temp directory (SSD) |

## Commands and results

Release:

```sh
cmake -S . -B build-final -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-final -j 8
ctest --test-dir build-final --output-on-failure
```

```
100% tests passed, 0 tests failed out of 7
unit 153 tests, 12342 checks, 0 failures
hooks 4 tests, 33 checks, 0 failures
fuzz_reader / fuzz_query / fuzz_json / fuzz_http / fuzz_roundtrip passed
```

Sanitizers (same test selection, hooks target omitted because ASan/TSan replace
the allocator):

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGBX_SANITIZER=address,undefined
cmake --build build-asan -j 8 && ctest --test-dir build-asan --output-on-failure
# 100% tests passed, 0 failed out of 6 (total 15.10 s)

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGBX_SANITIZER=thread
cmake --build build-tsan -j 8 && ctest --test-dir build-tsan --output-on-failure
# 100% tests passed, 0 failed out of 6 (total 18.00 s)
```

Fuzz modes are deterministic (fixed seeds) and ctest runs each with 160
iterations: mutated/truncated traces, mutated predicates, random JSON, random
HTTP requests, and random-trace save/load round-trips. The reader mode validates
invariants on every accepted trace and runs all analysis passes on it.

Coverage highlights:

- trace codecs, CRC, string tables, round-trips, corruption, truncation and every
  configured limit;
- recorder concurrency, drop accounting, snapshot splitting, sample truncation,
  live sink, cancellation, rejected calls after finish;
- allocation registry/tracker and opt-in `new`/`delete` hooks (plain builds);
- manual and Mach sampling, symbolization;
- span tree, active spans, timeline totals, series/state, heap reconstruction,
  leak ranking, flame graphs, dominators, deterministic diffs and cross-module
  property tests;
- query parsing/evaluation/limits, JSON writer/parser/limits;
- HTTP parsing/limits/SSE/security, workbench authentication, origin/host
  checks, path sandboxing (including symlink escapes), trace opening, every
  analysis route, export, state history, live demo streaming, shutdown, HEAD,
  connection caps, and browser-launch invocation.

## Benchmarks

Measured with `./build-final/bench/glassbox_bench` (Release, idle machine).
Numbers are runtime measurements, not estimates.

Recorder (100,000 iterations × 4 records; 8,192-slot ring; 1 MiB chunks):

```
397,813 records written in 0.275 s
1,444,652 records/s · 20.8 MB/s · dropped = 0
finish (drain + footer) 0.006 s
```

Synthetic trace (210,667 records; 3,255 KiB on disk), medians of 5 runs:

| Operation | Median |
|---|---:|
| Trace writer | 23.70 ms · 8.89M records/s |
| Trace reader | 25.0 ms · 8.42M records/s |
| In-memory analysis model | 47,104 KiB |
| Timeline (200 buckets) | 3.6 ms |
| Heap reconstruction at T/2 | 7.4 ms |
| Flame aggregation | 3.5 ms |
| Moment diff (two heaps + other analyses) | 19.9 ms |
| Query scan (limit 500) | 0.1 ms |

These support interactive scrubbing on traces of this size. Maximum-size traces
(2M records) and slower disks will scale up proportionally; see LIMITATIONS.md.
Methodology and raw context: `docs/benchmarks.md`.

## Size

Production C++ (public headers, library, launcher, examples), excluding tests,
fuzz, benchmarks and embedded UI assets:

| Module | LOC |
|---|---:|
| `include/glassbox/` (public API) | 1,664 |
| `src/trace/` | 1,639 |
| `src/recorder/` | 928 |
| `src/alloc/` | 503 |
| `src/sample/` | 316 |
| `src/analysis/` | 1,344 |
| `src/query/` | 778 |
| `src/ui/` (HTTP, SSE, JSON, routes, workbench) | 3,556 |
| `src/demo/` | 314 |
| `src/app/` (launcher) | 102 |
| `examples/` | 129 |
| **Production total** | **11,273** |

| Additional code | LOC |
|---|---:|
| `tests/` | 4,857 |
| `fuzz/` | 394 |
| `bench/` | 276 |
| UI assets (`ui/index.html`, `app.js`, `style.css`) | 2,950 |
| **Repository C++ total (incl. tests/fuzz/bench)** | **16,800** |

The production total slightly exceeds the 9–11k target because the loopback
server, JSON, security and workbench plumbing are part of that count; there is
no generated filler. UI assets are reported separately as required.

## Repository and delivery

- 63 commits before this report; this report is the 64th. Each slice was built
  and tested before committing (the repository has no CI by requirement).
- Golden demo trace: `examples/traces/demo.gbt`, 39,160 bytes, SHA-256
  `e8d78a8c6f40536ffffdaae3e5fc1ff02d15e256f87adcf3848b7d96bd957e95`.
- Public remote: `https://github.com/BViganotti/glassbox`, branch `main`.
- Binaries: `glassbox` (launcher), `glassbox_demo`, `glassbox_basic`,
  `glassbox_tests`, `glassbox_hooks_test`, `glassbox_fuzz`, `glassbox_bench`.
- No `.github/workflows`, no package manager files, no vendored sources.

## Known gaps and production assessment

The core engineering goals are met: bounded memory and parsing everywhere,
no known UB, deterministic analyses, explicit drop/truncation accounting, clean
sanitizer runs, loopback-only authenticated UI, and honest documentation of
approximations.

It is **not production grade as a general-purpose, distributable profiler**.
Specific gaps:

- **macOS only** (Mach sampler, `open`, BSD socket options, `arc4random_buf`);
  no Linux/Windows port.
- **No packaging/notarization/installer or auto-update**; it is built from source.
- **In-memory trace loading** with a 512 MiB file / 2M record ceiling; no mmap,
  compression, streaming analysis or multi-GB traces.
- **Sampling is approximate**: PCs plus validated frame-pointer walks, no
  DWARF/line info, no thread suspension, no unwinder. Stripped binaries show
  module offsets.
- **Allocation hooks are sharp**: global allocator replacement, no source lines,
  sampling makes heap figures approximate, incompatible with sanitizer
  allocators, and owner edges appear only when passed explicitly.
- **Dominator and list caps** are explicit; >20k-object graphs are truncated.
  Non-convergence is reported rather than hidden.
- **No soak testing** of multi-day recordings, and the deterministic fuzzers are
  not a replacement for continuous libFuzzer campaigns.
- **No CI/CD**, crash reporting, telemetry, accessibility audit, localization or
  multi-user/remote mode. The HTTP server is single-process and loopback-only,
  with no TLS (by design).
- **API stability**: headers are documented but there is no ABI/versioning
  promise beyond the trace format version.
- The UI is desktop-first; canvas interactions are mouse/keyboard oriented.

Verdict: suitable as a real local developer workbench for native C++ traces
within the documented limits, with strong soundness discipline and reproducible
verification. Shipping it to a broad audience would require cross-platform
support, packaging, longer stress/soak runs, and a hardened sampling story.

## Reproduction

```sh
cmake --preset release
cmake --build --preset release
ctest --test-dir build --output-on-failure
./build/glassbox_demo          # record the demo and open the workbench
./build/glassbox --trace examples/traces/demo.gbt
./build/bench/glassbox_bench
make asan
make tsan
make fuzz
```

The virtual quit control in the header stops the server; SIGINT/SIGTERM also
shut down cleanly. The trace file remains on disk.
