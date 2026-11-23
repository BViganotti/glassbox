# Glassbox — Final Report

Version 0.1.0 · 2026-11-23 · macOS (primary) and Linux (verified)

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
| 2 | Build succeeds with zero warnings | ✅ | Clean `build-final` build; `grep -c 'warning:'` → `0`; `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror` |
| 3 | `ctest` passes | ✅ | 7/7 in 13.09 s: 163 unit tests / 13,096 checks, 5 hooks tests, 5 fuzz modes |
| 4 | ASan/UBSan and TSan pass and are documented | ✅ | ASan+UBSan 6/6 (14.88 s), TSan 6/6 (18.19 s). Hooks excluded from sanitizer builds because sanitizers own the allocator |
| 5 | Launcher opens the UI; demo works | ✅ | Headless Chrome suite renders every view and a deep-linked query; browser spawn test intercepts `open`/`xdg-open`; golden demo trace; demo assertions for leak, spike and state drift |
| 6 | README, docs, CHANGELOG, LICENSE, LIMITATIONS, FINAL_REPORT | ✅ | All present, plus architecture, format, recorder, analysis, query, workbench, portability, packaging, security, testing, benchmarks and demo-walkthrough docs |
| 7 | FINAL_REPORT lists LOC, commits, tests, benchmarks, gaps | ✅ | This document |
| 8 | Repo committed and pushed | ✅ | `origin` = `github.com/BViganotti/glassbox`, branch `main` |
| 9 | No external dependency, no CI, no CLI-only product | ✅ | No third-party libraries/vendored code/network at runtime; no `.github/`; the product is the loopback workbench (examples are ordinary programs) |

Additional verification added beyond the original checklist:

- **Linux**: Ubuntu 24.04 arm64 container, GCC 13, clean build and `ctest` 7/7
  passed (`tools/linux_test.sh`).
- **Longer soak**: `tools/soak.sh` (reader 2,000 mutations, 20,000 query/JSON
  cases, 300 HTTP requests, 2,000 round-trips, five demo recordings) passed in
  18.3 s.
- **UI smoke**: every view, deep-linked query, quit endpoint and rendering
  (`tools/ui_smoke.sh`) passed.
- **Packaging**: `make bundle` produced and ad-hoc signed
  `dist/Glassbox.app`; `--version` runs from the bundle.

## Build and verification environment

| | |
|---|---|
| Host | macOS 26.7.3 / Darwin 25.6.0, arm64, 10 logical CPUs |
| macOS compiler | Apple Clang 21.0.0, CMake 4.2.3, Ninja 1.13.0 |
| Linux compiler | Ubuntu 24.04 container, GCC 13, CMake/Ninja from apt |
| Build flags | `-O3 -DNDEBUG`, libc++ hardening on macOS, warnings-as-errors |
| Trace storage | system temp directory / SSD |

## Commands and results

Release:

```sh
cmake -S . -B build-final -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-final -j 8
ctest --test-dir build-final --output-on-failure
```

```
100% tests passed, 0 tests failed out of 7
unit 163 tests, 13096 checks, 0 failures
hooks 5 tests, 39 checks, 0 failures
fuzz_reader / fuzz_query / fuzz_json / fuzz_http / fuzz_roundtrip passed
```

Sanitizers (hooks target omitted because ASan/TSan replace `operator new`):

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGBX_SANITIZER=address,undefined
cmake --build build-asan -j 8 && ctest --test-dir build-asan --output-on-failure
# 6/6 passed, 14.88 s

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGBX_SANITIZER=thread
cmake --build build-tsan -j 8 && ctest --test-dir build-tsan --output-on-failure
# 6/6 passed, 18.19 s
```

Linux:

```sh
tools/linux_test.sh
# builds glassbox-linux-builder (Ubuntu 24.04, GCC 13)
# 100% tests passed, 0 tests failed out of 7; unit 0.46 s
```

Browser/UI:

```sh
make smoke
# overview / flame / heap / spans / logs / state / diff / query / shutdown: ok
make bundle
# created dist/Glassbox.app (ad-hoc signed), version 0.1.0
```

Fuzz/soak:

```sh
./tools/soak.sh build-final
# reader 511 accepted / 1489 rejected; 3544 valid queries; 267 JSON docs;
# 300 HTTP responses with a healthy server; 2000 round-trips; demo loop ok
```

Coverage highlights:

- trace codecs, CRC, string tables, byte and streaming loaders, round-trips,
  truncation/corruption, memory/file/chunk/record/field/string limits;
- recorder concurrency, drop accounting, snapshot splitting, sample truncation,
  live sink, cancellation, rejected calls after finish, memory budgets;
- allocation registry, tracker, opt-in hooks (sampling, min-size, tracked frees);
- manual sampling, Mach sampling with thread suspension and safe reads,
  symbolization; Linux manual fallback;
- span tree, active spans, timeline totals, series/state/history, heap
  reconstruction, leak ranking, flame graphs, dominators, deterministic diffs,
  cross-module property tests;
- query parsing/evaluation/limits, JSON writer/parser/limits;
- HTTP parsing/limits/SSE/security, workbench auth, origin/host checks, path and
  symlink sandboxing, trace opening, analysis routes, export, state history,
  live demo, shutdown, HEAD, connection caps, browser spawn, platform seam.

## Benchmarks

Measured with `./build-final/bench/glassbox_bench` (Release, idle machine):

Recorder (100,000 iterations × 4 records; 8,192-slot ring; 1 MiB chunks):

```
399,386 records written in 0.274 s
1,455,388 records/s · 20.9 MB/s · dropped = 0
finish (drain + footer) 0.008 s
```

Synthetic trace (210,667 records; 3,255 KiB on disk), medians of 5 runs:

| Operation | Median |
|---|---:|
| Trace writer | 34.3 ms · 6.14M records/s |
| Trace reader (streaming) | 26.1 ms · 8.07M records/s |
| In-memory analysis model | 47,104 KiB |
| Timeline (200 buckets) | 3.7 ms |
| Heap reconstruction at T/2 | 7.4 ms |
| Flame aggregation | 3.6 ms |
| Moment diff (two heaps + analyses) | 20.0 ms |
| Query scan (limit 500) | 0.1 ms |

Workstation measurements; slower disks and hooks-enabled recording reduce
throughput. The loader now streams chunks from disk, so its peak memory is the
decoded model plus one payload (≤ 8 MiB), not the whole file. Methodology:
`docs/benchmarks.md`.

## Size

Production C++ (public headers, library, launcher, examples), excluding tests,
fuzz, benchmarks and embedded UI assets:

| Module | LOC |
|---|---:|
| `include/glassbox/` (public API) | 1,680 |
| `src/trace/` (format, streaming I/O) | 1,984 |
| `src/recorder/` | 926 |
| `src/alloc/` | 506 |
| `src/sample/` | 385 |
| `src/analysis/` | 1,344 |
| `src/query/` | 778 |
| `src/ui/` (HTTP, SSE, JSON, routes, workbench) | 3,566 |
| `src/demo/` | 314 |
| `src/platform/` | 153 |
| `src/app/` (launcher) | 102 |
| `examples/` | 129 |
| **Production total** | **11,867** |

| Additional code | LOC |
|---|---:|
| `tests/` | 5,125 |
| `fuzz/` | 415 |
| `bench/` | 276 |
| UI assets (`ui/index.html`, `app.js`, `style.css`) | 2,969 |
| **Repository C++ total (incl. tests/fuzz/bench)** | **17,683** |

The production total is slightly above the 9–11k target because the workbench
server, JSON, security, platform and packaging plumbing are included; there is
no generated filler. UI assets are reported separately as required.

## Repository and delivery

- 83 commits before this report; this report is the 84th. Each slice was built
  and tested before committing; there is no CI by project rule.
- Golden demo trace: `examples/traces/demo.gbt`, 39,160 bytes, SHA-256
  `e8d78a8c6f40536ffffdaae3e5fc1ff02d15e256f87adcf3848b7d96bd957e95`.
- Public remote: `https://github.com/BViganotti/glassbox`, branch `main`.
- Binaries: `glassbox` (launcher), `glassbox_demo`, `glassbox_basic`,
  `glassbox_tests`, `glassbox_hooks_test`, `glassbox_fuzz`, `glassbox_bench`.
- Packaging: `dist/Glassbox.app` via `make bundle` (ad-hoc signed; optional
  Developer ID via `GBX_CODESIGN_IDENTITY`).
- No `.github/workflows`, no package manager files, no vendored sources.

## Known gaps and production assessment

The engineering goals are met and the original gaps were reduced materially:
streaming I/O with a real memory budget, Linux support verified in a container,
thread suspension for Mach sampling, hook size filtering, app-bundle packaging,
browser smoke automation, larger configurable caps, and a local release/soak
gate.

It is still **not production grade as a broadly distributable commercial
profiler**:

- **Windows is unsupported**, and Linux packaging is source-only. macOS is the
  primary target.
- **No notarization/stapling, auto-update, universal binary or signed
  distribution.** The `.app` is ad-hoc signed unless a Developer ID is supplied.
- **Analysis is still in-memory**: 2 GiB file / 1 GiB decoded-memory defaults.
  A multi-gigabyte trace that exceeds the model budget fails cleanly rather
  than offering disk-backed windowed analysis. There is no compression or mmap.
- **No DWARF/line-number resolution and no CFI unwinder.** Sampling uses
  `dladdr` symbols plus validated frame-pointer chains; stripped binaries show
  module offsets. This is the largest sampling gap. Linux has no Mach sampler.
- **Allocation hooks are inherently sharp**: global allocator replacement,
  symbol-level sites, sampling/min-size approximations, sanitizer-incompatible,
  and owner edges only when supplied by the caller.
- **Analysis caps still exist** (50k graph nodes, 100k flame nodes, 20k heap
  objects, 5k events) even though they are larger and configurable per request.
- **No CI/CD** (required), no crash reporting/telemetry, no accessibility
  audit/localization, and the API has no ABI/versioning promise beyond trace
  format v1.
- **UI automation** covers rendering of every view, deep-linked query, quit
  route and launch; it does not click every button (endpoints are API-tested).
- The UI is desktop-first; canvas interactions are mouse/keyboard oriented.

Verdict: a sound, verified local workbench for macOS and Linux traces within the
documented limits, with reproducible local QA. It is not yet a notarized,
cross-platform, disk-backed commercial profiler; the remaining work is
distribution and deeper symbol/unwind infrastructure rather than core
soundness.

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
make linux-test
make smoke
make soak
make bundle
make verify                    # full local release gate
```

The header power button stops the server; SIGINT/SIGTERM also shut down cleanly.
The trace file remains on disk.
