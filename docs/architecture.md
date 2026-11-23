# Architecture

Glassbox is one static library (`glassbox`), one optional allocator-hook library
(`glassbox_hooks`), and three executables (launcher, demo, basic example). The
library has no third-party dependencies: C++20, POSIX and macOS system APIs.

## Module graph

```
include/glassbox/*.hpp        public API (documented, stable-ish)
        |
        +-- trace/       model, varint, CRC, file I/O, writer, reader, format
        +-- recorder/    bounded ring, writer thread, spans, live sink
        |      |
        |      +-- alloc/      registry, tracker, hook runtime
        |      +-- sample/     manual + Mach sampling, symbols
        |
        +-- analysis/    spans, timeline, series/state, heap, flame,
        |                dominators, diff
        +-- query/       lexer, parser, evaluator
        +-- demo/        scripted workload (uses recorder + analysis helpers)
        +-- ui/          JSON, HTTP/SSE server, auth, trace registry, routes,
                         workbench lifecycle, browser launch, assets
```

Dependency direction is one-way: `ui` depends on `analysis`, `query`, `trace`,
`recorder`, `demo`; `analysis` depends only on `trace`; `recorder` depends on
`trace` and the allocator suppression bridge; `trace` depends on nothing but the
standard library and POSIX file APIs. No layer includes a UI header.

## Data flow

```
application ── > Recorder (producer threads)
                    │  fixed ring, strings interned
                    ▼
                writer thread ──> TraceWriter ──> .gbt chunks + footer
                    │
                    └─ live sink ──> SSE clients

.gbt file ──> reader ──> Trace (flat records + fields + strings)
                            │
             ┌──────────────┼────────────────┐
             ▼              ▼                ▼
        spans/timeline   heap/dominators   flame/diff/query
             └──────────────┴────────────────┘
                            ▼
                   HTTP JSON routes ──> workbench UI (canvas/DOM)
```

The file is the source of truth. The UI never holds mutable analysis state;
every endpoint reconstructs its view from the immutable `Trace` snapshot under
a mutex-held `shared_ptr`. Opening a new trace swaps the pointer atomically from
the reader's perspective.

## Threading

| Thread | Responsibility | Shared state |
|---|---|---|
| application threads | produce records | recorder ring + string table (mutex) |
| recorder writer | encode chunks, file I/O, live sink | string table, live feed |
| HTTP accept | accept + dispatch | route map (immutable after start) |
| HTTP workers | one request each; SSE long-lived | trace registry, live feed |
| demo recorder | record demo, publish SSE | recorder, trace registry |
| Mach sampler | read thread states, emit samples | recorder (thread-safe) |
| benchmark/fuzz | single-threaded drivers | n/a |

Lock order: recorder string table → ring; live feed is independent; trace
registry is independent; HTTP route map is copied under a short lock. No lock is
held while running a user handler or writing a socket. HTTP workers are
detached but counted; stopping waits for the count to reach zero, so captured
workbench state cannot be destroyed under a live handler.

## Bounded by construction

Every collection has a cap and every parser validates lengths before allocating:

- trace: file/records/fields/strings/chunks/thread limits (see
  [trace-format.md](trace-format.md));
- recorder: fixed ring, max 24 fields per pending record, capped strings and
  frames;
- heap/dominators/flame/spans/lists: per-request caps reported as truncation;
- query: source/token/depth/literal limits;
- HTTP: header/body/header-count/query-param/connection/response caps;
- JSON: bytes/depth/string/array/object caps;
- UI: each view requests bounded limits and shows truncation notices.

## Assets and the launcher

`cmake/EmbedAssets.cmake` reads `ui/*` as hex at configure time and emits
`embedded_assets.cpp`; there is no build-time binary generator. The launcher
creates a `Workbench`, which generates a token, binds loopback (7331..7340 then
ephemeral), registers routes, loads the newest trace or records the demo, starts
the accept thread, then runs `posix_spawnp("open", ...)` with the tokenized URL.
The main loop sleeps and watches the running/shutdown flags; the UI's shutdown
route, SIGINT or SIGTERM stops it.

## Trace file ownership

The writer streams chunks and never seeks; the footer is the only final
structure. A crashed writer leaves a footer-less file that the reader recovers
with a warning. Reading is all-or-error: a corrupt chunk aborts the load, while
an incomplete tail is either recovered (default) or rejected
(`recover_truncated=false`).

## Testing shape

`tests/` contains one unit binary (globbed sources) plus a separate hooks test
that links the allocator replacement library. `fuzz/` is a single executable
with deterministic modes; `bench/` is a normal executable, not a ctest target.
Sanitizer builds reuse the same tree with `GBX_SANITIZER`. There are no CI
workflows in the repository.

## Public API surface

Headers under `include/glassbox/`:

- `glassbox.hpp` — umbrella include.
- `status.hpp` — error codes and status.
- `attributes.hpp` — record/field/value model, well-known keys.
- `trace.hpp` — string table, trace metadata, records, snapshots.
- `trace_io.hpp` — save/load options, current process metadata.
- `recorder.hpp` / `span.hpp` — recording API and RAII spans.
- `alloc.hpp` / `alloc_hooks.hpp` — tracking and opt-in interception.
- `sample.hpp` — manual/Mach sampling and symbolization.
- `analysis.hpp` — every analysis data type and function.
- `query.hpp` — predicate language.
- `json.hpp` — bounded JSON writer/parser used by the server (also handy for
  embedding).
- `demo.hpp` — the demo workload.
- `workbench.hpp` — the local server/launcher lifecycle.

Types use value semantics where practical; long-lived server state is hidden
behind a pimpl in `Recorder`, `Workbench`, `MachSampler` and `ObjectRegistry`.
