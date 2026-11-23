# Glassbox — Implementation Plan

Status: ready for one-shot implementation in this repository.

## Mission
Glassbox is a time-travel inspector for native C++ applications. It records CPU
samples, heap allocations, spans, logs, counters and state snapshots, then lets a
developer scrub backward and forward through an execution: what was alive at
time T, which code allocated it, which span was active, and what changed between
two moments. It is a local desktop workbench, not a CLI and not a toy.

## Product shape
- Embeddable `glassbox` C++ library.
- Local UI workbench launched by a small executable.
- Demo app with a deliberate leak, a latency spike and a state bug.
- Every trace is a portable file that can be reopened offline.

## Architecture
```
glassbox/
  include/glassbox/
    recorder.hpp, span.hpp, attributes.hpp, snapshot.hpp, alloc.hpp
  src/
    trace/       model, string table, varint, CRC, writer/reader, indexes
    recorder/    bounded queue, drop policy, spans, logs, counters
    alloc/       allocation tracking, opt-in hooks, object registry
    sample/      manual sampling, optional Mach sampler, symbolization
    analysis/    flame graph, heap graph, dominators, retained size, diff
    ui/          HTTP/SSE, JSON, embedded assets, launcher
    query/       predicate parser and evaluator
  ui/            original HTML/CSS/JS assets, embedded at build time
  examples/glassbox_demo/
  tests/  fuzz/  bench/
```

## LOC budget (target ~10k)
| Module | LOC |
|---|---:|
| trace format/writer/reader | 1,600 |
| recorder/ring/API | 1,400 |
| allocation + object tracking | 1,000 |
| sampling + symbols | 900 |
| analysis (heap/flame/diff) | 1,800 |
| query language | 700 |
| UI server + assets | 1,800 |
| launcher + demo | 500 |
| tests/fuzz/bench | 1,300 |

## Milestones and commit ranges
### P0 Scaffold (commits 1–8)
CMake, test harness, README, LICENSE, Makefile, directory layout, no CI, no dependencies.

### P1 Trace format (commits 9–20)
Event/span/attribute model → binary writer/reader → varint/CRC → string table → chunk index → random access → truncation recovery → golden traces → fuzz reader.

### P2 Recorder (commits 21–30)
Public API → bounded ring → multi-producer append → drop policy/accounting → span tree → logs/counters/gauges → live stream → recorder benchmarks.

### P3 Allocation and object tracking (commits 31–40)
`track_alloc`/`track_free` → strictly opt-in new/delete hooks → allocation timeline → object registry → snapshots → snapshot diff.

### P4 Sampling (commits 41–48)
Manual sample API first → macOS Mach sampler if safely possible → stack capture → `dladdr` symbols → aggregation → overhead benchmark. If Mach sampling is unsafe, ship manual sampling and document it.

### P5 Analysis (commits 49–60)
Time buckets → intervals → flame graph → heap graph → retained size → dominator tree → leak candidates → deterministic diff.

### P6 UI (commits 61–74)
HTTP/JSON/SSE → embedded assets → timeline → scrubber → flame canvas → heap table/treemap → object inspector → query bar → diff view. Mouse/keyboard only.

### P7 Demo and hardening (commits 75–90)
Demo with leak, latency spike and state bug → golden demo trace → malformed trace fuzz → ASan/UBSan → TSan → benchmarks → docs → FINAL_REPORT.

## Testing
- Round-trip trace tests and golden corpus.
- Property tests for varint, string table, ring ordering and diff.
- Fuzz trace reader, query parser and UI request parser.
- Heap graph and dominator invariants.
- Recorder overhead benchmark and live-stream test.

## Acceptance criteria
- Open a trace, scrub to a timestamp, see the live heap and active spans.
- Compare two snapshots and identify the leaking allocation site.
- Flame graph attributes CPU time to functions.
- Malformed/truncated traces fail cleanly.
- Recorder overhead measured and documented.
- UI is offline, loopback-only, asset-complete and non-CLI.

## Risks and fallbacks
- Mach sampling permissions: ship manual sampling plus `dladdr` and document.
- Heap hooks are dangerous: require opt-in; never break host apps silently.
- UI scope: local web UI with canvas is the required baseline.
