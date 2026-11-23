# Public API reference (map)

This is a navigation aid; each header carries the authoritative doc comments.
Threading/lifetime rules are repeated where they matter.

## Status and model

`status.hpp`

- `gbx::ErrorCode`, `gbx::Status` (`ok`, `code`, `message`, `describe`),
  `gbx::wrap`.
- No public API throws for malformed data; failures are returned.

`attributes.hpp`

- `RecordType` (13 kinds), `LogLevel` (0..5), `ValueType`, `Value`, `Field`,
  `Record`, `keys::*`, `kMaxRecordFields`, `kFrameSeparator`.
- Records reference fields by offset into `Trace::fields` and strings by id
  into `Trace::strings`.

`trace.hpp`

- `StringTable` (bounded interning), `TraceMeta`, `ChunkInfo`, `TraceLimits`,
  `TraceDiagnostics`.
- `Trace`: owned metadata, records, fields, strings, chunk index, diagnostics.
  Methods: `add_record`, `finalize`, `field`, `int_field`, `double_field`,
  `str_field`, `thread_name`, `record_range`, `memory_usage`.
- `SnapshotView` for iterating snapshot key/value pairs.

`trace_io.hpp`

- `SaveOptions`, `LoadOptions`, `current_meta`, `save_trace`, `load_trace`,
  `load_trace_bytes`.

## Recording

`recorder.hpp`, `span.hpp`

- `RecorderConfig`, `DropPolicy`, `RecorderStats`, `LiveField`, `LiveRecord`,
  `LiveSink`.
- `Recorder`: start/finish/cancel, log, counter, gauge, mark, snapshot,
  snapshot_text, alloc, free, sample, thread_name, span_begin/end,
  begin_span, current_span, thread_slot, stats, error, set_live_sink.
- `Span`: move-only RAII span with `end`, `active`, `id`, `log`, `counter`.
- `GBX_SPAN` convenience macro.

`alloc.hpp`, `alloc_hooks.hpp`

- `SourceLoc`, `GBX_HERE`, `AllocationSummary`, `AllocationSiteSummary`.
- `ObjectRegistry` (bounded, thread-safe live objects).
- `AllocationTracker` (emit alloc/free + registry).
- `HookConfig`, `install_allocation_hooks`, `uninstall_allocation_hooks`,
  `allocation_hooks_installed` (only functional when linking `glassbox_hooks`).

`sample.hpp`

- `SymbolInfo`, `symbolize_address`, `symbolize_pointer`, `short_symbol`.
- `ManualSampler` (`capture`, `record`).
- `MachSamplerConfig`, `MachSamplerStats`, `MachSampler` (start/stop/stats).
- `MachSampler` is macOS-only by implementation.

## Analysis and query

`analysis.hpp`

- `TimeWindow`, `SpanInfo`, `SpanAnalysis`, `ActiveSpan`, `analyze_spans`,
  `active_spans`.
- `TimelineBucket`, `Timeline`, `build_timeline`.
- `SeriesPoint`, `Series`, `counter_series`, `series_value_at`.
- `StateEntry`, `state_at`, `StatePoint`, `state_history`,
  `format_state_entry`.
- `HeapAllocation`, `HeapSiteStat`, `HeapObjectView`, `HeapAt`, `HeapOptions`,
  `heap_at`, `live_allocations`, `LeakCandidate`, `leak_candidates`.
- `FlameNode`, `FlameGraph`, `FlameOptions`, `build_flame`,
  `build_span_flame`.
- `HeapGraphNode`, `HeapGraphOptions`, `HeapGraph`, `build_heap_graph`.
- `DiffResult` and its delta structs, `diff`.

`query.hpp`

- `QueryError`, `Query` (`parse`, `valid`, `matches`, `describe`, `node_count`),
  `run_query`.

`json.hpp`

- `json::Writer` (always valid JSON, escapes invalid UTF-8, safe integers as
  strings beyond 2^53), `json::quote`, `json::Value`, `json::ParseLimits`,
  `json::parse`.

## Workbench and demo

`workbench.hpp`

- `WorkbenchOptions`, `Workbench` (`start`, `stop`, `running`, `port`, `url`,
  `token`, `open_trace`, `last_error`), `open_in_browser`.

`demo.hpp`

- `DemoOptions`, `DemoResult`, `run_demo`, `record_demo_trace`.

## Lifetimes

- A `Recorder` outlives every `Span` created from it; spans are not thread-safe
  handles, but the recorder methods are.
- A `Trace` owns its arrays; pointers/values returned by accessors are valid
  until the trace is destroyed or mutated via `add_record`/`finalize`.
- `Workbench` joins its worker threads in `stop`/destruction; do not call
  `stop` from inside an HTTP handler (the shutdown route simply requests stop).
- `Install` hooks after starting a recorder and uninstall before finishing it.
- `MachSampler` must be stopped before the recorder finishes.
