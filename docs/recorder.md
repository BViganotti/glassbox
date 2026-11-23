# Recorder API

`gbx::Recorder` turns application activity into a trace. It is a bounded,
multi-producer recorder with a single writer thread; the host application never
blocks on disk I/O unless it explicitly selects the blocking drop policy.

## Lifecycle

```cpp
#include <glassbox/glassbox.hpp>

gbx::Recorder rec;
auto status = rec.start("traces/session.gbt");
if (!status.ok()) { /* report */ }

{
  auto span = rec.begin_span("Frame", "cpu");   // RAII: ends on scope exit
  rec.log(gbx::LogLevel::Info, "tick", "engine");
  rec.counter("fps", 59.8, "hz");
  rec.snapshot({{"entities", 42}, {"health", 100.0}});
}

status = rec.finish();  // drains the queue, writes the footer, joins the writer
```

- `start` opens the file, writes the header, and spawns the writer thread.
- `finish` drains pending records, writes a loss-accounting marker if anything
  was dropped, writes the footer and joins the writer. It is idempotent.
- `cancel` stops immediately; the partial file has no footer and opens as a
  truncated trace.
- A recorder can be started once. Starting twice returns `Busy`.
- The destructor calls `finish` if the recorder is still running.

Recording calls made before `start` or after `finish` are rejected and counted
in `RecorderStats::records_rejected`; they never throw.

## Threading model

- Every recording method is safe to call concurrently from any number of
  threads.
- `start`, `finish`, `cancel` and `set_live_sink` must not race each other.
- Each thread gets a dense slot id and emits a thread-name record on its first
  event. `thread_name()` overrides the default (`pthread_getname_np` or
  "Thread N").
- Span nesting is tracked per thread and per recorder instance. Spans need not
  be strictly balanced; `span_end` removes the matching id from the thread's
  stack and emits an end record even for unknown ids (analysis counts those as
  unmatched ends rather than inventing a tree).
- Timestamps come from `CLOCK_MONOTONIC` and are relative to `start`. Records
  are allowed to be slightly out of order across threads; the trace is stably
  sorted by timestamp when loaded.

Recording methods allocate only for string interning and the snapshot vector;
their internal allocations suspend the optional allocation hooks so a hook can
never recurse into the recorder.

## Bounded queue and drop policy

Pending records live in a fixed ring (`RecorderConfig::ring_capacity`, clamped
to 64..262,144 slots). Each slot is a fixed-size record with up to 24 fields;
snapshot records with more fields are split into consecutive snapshot records
at the same timestamp.

When the ring is full:

- `DropPolicy::DropNewest` (default) drops the incoming record, increments
  `records_dropped`, and increments `queue_full_events`. The host never blocks.
- `DropPolicy::Block` waits for space. A writer failure wakes blocked producers
  and they drop instead of hanging.

Loss is never silent. If any records were dropped, the writer appends a
`marker` record before the footer with `dropped_records`, `dropped_strings` and
`rejected_records` fields (keys reserved at startup, so they survive an
exhausted string table). The workbench shows the marker in the event stream.

## Strings

Keys and string values are interned into a table bounded by
`max_strings` (default 262,144) and `max_string_bytes` (default 32 MiB). If a
string cannot be interned the record is dropped and counted; the trace never
contains dangling string ids.

## Limits

| Setting | Default |
|---|---:|
| ring capacity | 8,192 records |
| chunk bytes | 1 MiB |
| records per chunk | 50,000 |
| fields per pending record | 24 (snapshots split) |
| frame count per sample | 64 |
| frame bytes per sample | 1 KiB per frame, 8 KiB total |
| thread slots | 65,536 |

Per-frame samples that exceed the frame budget set `truncated=true` on the
record so consumers know the stack was shortened.

## Spans and records

`span_begin` returns a monotonically increasing id. `begin_span` wraps it in a
move-only `Span` that ends on destruction:

```cpp
auto span = rec.begin_span("Decode", "io");
span.log(LogLevel::Debug, "chunk read");
span.counter("bytes", 4096);
```

`current_span()` returns the innermost open span on the calling thread (0 when
none). An explicit parent id can be passed to `span_begin`; it is used for the
`parent` field and does not change the thread-local stack except that the new
span becomes innermost.

## Live events

`set_live_sink(LiveSink)` receives fully resolved records from the writer
thread. The sink runs without internal locks held; it must not throw and should
be quick. The workbench uses it to feed the SSE stream.

```cpp
rec.set_live_sink([](const gbx::LiveRecord& event) {
  // event.ts, event.type, event.thread_name, event.fields[].key/text/value
});
```

If a sink is slow, the recorder slows down; the queue still absorbs bursts and
reports drops rather than growing without bound.

## Allocation tracking

`gbx::AllocationTracker` wraps a recorder and keeps a bounded registry of live
objects for owner inference:

```cpp
gbx::AllocationTracker tracker(rec, /*max_objects=*/100000);
auto* mesh = new Mesh();
tracker.alloc(mesh, sizeof(Mesh), GBX_HERE, "Mesh");
tracker.free(mesh);
delete mesh;
```

The trace is the source of truth; the registry may stop tracking after
`max_objects` and reports the overflow count. `free_tracked` (used by the
hooks) emits a free record only for allocations the recorder actually saw.

## Opt-in allocator hooks

`glassbox_hooks` is a separate static library that replaces global
`operator new/delete`. Linking it is the opt-in switch; nothing is recorded
until `install_allocation_hooks` is called.

```cpp
gbx::HookConfig config;
config.recorder = &rec;
config.sample_every = 16;
gbx::install_allocation_hooks(config);
// ... run the application ...
gbx::uninstall_allocation_hooks();
rec.finish();
```

Rules and caveats:

- Start the recorder first, uninstall the hooks before finishing it.
- Install/uninstall when other threads are quiescent; the hook runtime is
  process-global.
- Allocation sites are symbolized with `dladdr` (symbol name plus module
  offset), not source lines.
- `sample_every > 1` records one allocation out of N. Frees are only recorded
  for tracked pointers, so the heap view is a sample.
- Do not link the hook library together with ASan/TSan; they own the allocator.
  The test suite builds the hooks test only in plain builds.
- Glassbox's own bookkeeping suppresses interception on the recording thread,
  so the recorder cannot feed itself.

## Errors

Every fallible operation returns `gbx::Status` with an `ErrorCode`. Recording
methods cannot fail; `error()` and `stats()` expose writer failures and loss
accounting. `finish()` returns the writer's status, including I/O errors and
any failure to write the footer.
