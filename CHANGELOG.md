# Changelog

All notable changes to Glassbox are documented here. The project follows a
simple "version, date, sections" format.

## 0.1.0 — 2026-11-23

First complete release: recorder, trace format, analysis, workbench and demo.

### Added

- Portable `.gbt` trace format: versioned header, CRC-32 checked chunks,
  per-chunk string tables, varint/zig-zag encoding, footer with counts and
  duration, bounded decoding, and truncation recovery diagnostics.
- `gbx::Recorder`: fixed-size multi-producer ring, writer thread, drop policy
  with accounting, persisted loss marker, per-thread spans, logs, counters,
  gauges, snapshots, samples and markers.
- `gbx::Span` RAII handle and thread-local nesting.
- `gbx::AllocationTracker` with a bounded live-object registry and owner edges.
- Optional `glassbox_hooks` library replacing global `new`/`delete`, with
  `dladdr`-based allocation sites, sampling, recursion suppression and a
  dedicated test binary.
- Sampling: manual frame-pointer stack capture, Mach thread-state sampler for
  this process with safe remote reads, symbolization and C++ demangling.
- Analysis: span tree and active spans, timeline buckets, counter series,
  snapshot state replay, heap reconstruction, leak candidates, sample and
  span-based flame graphs, Cooper–Harvey–Kennedy dominators with retained
  sizes, and deterministic A/B diffs.
- Query language with type/field predicates, durations, text search, boolean
  logic, precedence and hard parser limits.
- State-key history endpoint and inspector chart; moment export (heap, state,
  active spans, leaks) as downloadable JSON.
- Shareable UI deep links for the view, scrub position and A/B moments; explicit
  quit control; demo seed selection in the UI and CLI.
- Browser-launch test that intercepts `open` and checks the tokenized URL.
- Loopback HTTP/1.1 server with SSE, random session token, cookie bootstrap,
  Host/Origin validation, CSP and security headers, bounded requests and
  responses, trace root sandboxing, and a trace registry.
- Workbench UI: timeline with zoom/scrub/A-B markers, overview dashboard,
  flame icicle, heap sites/treemap/objects, retained graph, span waterfall and
  tree, logs, state, diff, query table, inspector drawer, keyboard shortcuts,
  deep links via the URL hash, live demo recording with progress.
- Browser launcher using macOS `open`; `--no-browser` for automation.
- Demo workload with a real leak, a latency spike, ownership hierarchy and a
  drifting inventory state; committed golden trace.
- Tests: 120+ unit tests, HTTP/SSE integration tests, deterministic fuzzers for
  reader/query/JSON/HTTP/roundtrip, and a benchmark suite.
- Documentation for the trace format, recorder, analysis, query, workbench,
  testing and limitations.

### Changed

- Accepted sockets explicitly clear the nonblocking flag inherited from the
  listener on macOS so request reads honour the configured timeout.
- Unauthenticated SSE clients now receive a real HTTP 403 before any stream
  headers, instead of a 200 stream carrying an error event.
- Trace listing and newest-trace selection resolve symlinks before checking the
  allowed roots, so escaping links are neither listed nor auto-opened.
- The reader validates the footer duration against the last record and records
  true per-chunk timestamp ranges.
- `query.describe()` prints colon equality as `=` for canonical output.

### Fixed

- Live SSE subscribers no longer miss the terminal `done` event when the event
  feed is delivered in batches.
- The overview event feed is scoped to the scrub position rather than showing
  end-of-trace events.
- Deep-link scrub parameters are preserved when the launcher token is removed
  from the URL.
- HTTP request fuzzing uses short timeouts so malformed requests cannot stall
  the suite.
- Wall-clock rendering parses the large `wall_start_ns` value from JSON without
  string concatenation.

### Verified

- Clean Release build with zero compiler warnings.
- `ctest`: 152 unit tests (12,127 checks), 4 hooks tests, five fuzz modes.
- ASan+UBSan and TSan runs across the full suite (hooks excluded by design).
- Recorder/trace/analysis benchmarks recorded in `docs/benchmarks.md` and
  `FINAL_REPORT.md`.
- Launcher URL rendering checked with headless Chrome; browser spawn checked by
  a test that intercepts the macOS `open` command.
