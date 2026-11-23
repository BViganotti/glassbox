# Limitations and known gaps

Glassbox aims to be sound before fast. This file lists what it does not do and
where its approximations live. Nothing here is hidden from the UI: warnings and
truncation flags are surfaced, and drops are persisted in the trace.

## Platform

- **macOS only.** The Mach sampler, `posix_spawn` browser launch, `SO_NOSIGPIPE`
  and `arc4random_buf` are macOS/BSD APIs. There is no Linux or Windows port.
- Apple Clang/libc++ is the tested toolchain. Other compilers are untested.
- The UI targets desktop browsers with canvas and SSE. Touch works through
  pointer events but is not specifically designed.

## Trace loading and storage

- The reader streams chunks (peak memory is the decoded model plus one 8 MiB
  payload) but still builds a flat in-memory record array for analysis. There
  is no mmap, compression or on-disk query engine. Very large traces are
  bounded but memory-hungry (~48 MiB of analysis trace for 210k dense records
  in the benchmark).
- The file limit is 2 GiB, the decoded-memory budget is 1 GiB by default, and
  the record/field caps are 8,000,000/32,000,000. Beyond those the trace is
  rejected with `LimitExceeded`; it is not silently truncated. A 2 GiB trace
  that decodes past the memory budget cannot be inspected in full.
- Chunk decoding requires reasonable compression of repeated keys; strings are
  deduplicated per chunk, not globally, so a pathological writer can spend more
  space.
- Format version 1 is fixed. Unknown flags and extra metadata are ignored, but
  there is no migration tool for future versions.
- Trace indexing in memory is by parsed timestamps, not by file offset. The
  file keeps chunk offsets for tools, but the UI always loads the full trace.

## CPU sampling

- The Mach sampler reads program counters and optionally walks frame pointers.
  It never suspends the target threads while sampling, so a stack can be
  slightly torn or stale. Reads are done with `mach_vm_read_overwrite` and
  validated (alignment, monotonic frame pointers, one-megabyte span), so bad
  frames end the walk rather than crashing.
- There is no DWARF/line-number resolution. Symbols come from `dladdr`
  (demangled when possible), so stripped binaries show module+offset. Function
  names can be mangled-looking when the ABI demangler cannot resolve them.
- Manual samples require frame pointers for more than the immediate caller.
  Optimized code without frame pointers yields shallow stacks. The demo and
  tests are built with the platform default (frame pointers on arm64).
- Samples carry weights but the UI does not currently implement time-weighted
  aggregation beyond summing weights.

## Allocation tracking and hooks

- Ownership (`owner`) is only recorded when the caller passes it. Neither the
  hooks nor the tracker infer object graphs automatically.
- The opt-in hooks replace the process-wide allocator and must be installed
  while other threads are quiescent. They are incompatible with sanitizer
  allocators and are not linked into the main binary.
- Hook sites are `dladdr` symbols, not source lines. `sample_every > 1` makes
  the heap view a sample; frees are only emitted for tracked pointers, so
  sampled traces do not aim for exact totals.
- The object registry is bounded (default 100,000 live objects); past that,
  tracking stops and the overflow count is reported.
- The demo's "leaks" are intentional real allocations freed right after the
  trace closes. The trace shows them live at the end; they are not a claim that
  the workbench can prove a leak in arbitrary code.

## Heap graph and dominators

- Retained size is computed over the ownership edges present in the trace.
  Missing owner information makes objects roots, which can overstate what is
  reachable from the real application roots.
- Graph analysis is capped (default 20,000 nodes / 100,000 edges). Larger heaps
  are truncated and the flag is surfaced. If the iterative dominator algorithm
  does not converge within 1,000 rounds (malformed cycles), `converged=false`
  is reported and the last safe numbers are returned.
- Heap reconstruction is O(records) per query and per diff side. With the
  maximum trace size this can take hundreds of milliseconds; there is no
  incremental heap index or checkpoint cache.

## Recorder

- Pending records are limited to 24 fields. Snapshot calls with more keys are
  split across multiple snapshot records at the same timestamp; custom records
  with more fields are not supported by the public API.
- When the ring is full, `DropNewest` loses records (reported and persisted).
  `Block` can stall the host if the writer is slow.
- The writer holds the string-table lock while encoding a record, so string
  interning and writing serialize. This is a deliberate simplicity trade-off;
  the benchmark shows ~1.4M records/s on the test machine.
- String interning failure drops the record (accounted), never emits dangling
  ids.
- Timestamps come from a monotonic clock; wall time is derived from a single
  start instant and is not adjusted for clock changes during recording.

## UI and server

- Loopback only, one process, one user. No TLS, no multi-user isolation, no
  remote access. The random token is per process and lives in the URL.
- One request per connection; there is no keep-alive. SSE streams are
  close-delimited rather than chunked.
- The live feed is bounded at 1,024 events; slow UI clients receive a `gap`
  notice and lose events. Completed recordings are always loaded from the trace
  file, so gaps do not affect final analysis.
- Response sizes are capped (32 MiB) and list endpoints have their own limits
  (events 1,000, objects 5,000, spans 20,000, flame nodes 30,000, logs 500).
  The UI reports truncation.
- The flame canvas draws a fixed height and clips very deep stacks; the summary
  reports depth truncation, and the data itself is capped by the API.
- Bookmark/state persistence, trace editing, export, and multi-trace comparison
  are not implemented.
- The UI uses self-hosted assets only; no themes, plugins or localization.

## Query

- No regex, arithmetic, aggregation or joins. String ordering is lexicographic
  and case-insensitive equality/substring is ASCII-only (UTF-8 content is
  compared bytewise after ASCII lowercasing).
- Query results are a single-page list (default 500, hard max 10,000); there is
  no pagination cursor.
- Fields compare only against their stored type: a string value will not equal
  a numeric literal even if it looks numeric.

## Build, CI and delivery

- There is intentionally no CI/CD configuration. Verification is local
  (`ctest`, sanitizer builds, fuzzers, benchmarks).
- The build requires CMake and Ninja; no Makefile-only or IDE project files are
  generated.
- Assets are re-embedded at CMake configure time; editing `ui/*` triggers a
  reconfigure but not a watch loop.
- The repository includes a generated golden trace (`examples/traces/demo.gbt`);
  regenerating it changes its bytes while tests only assert structure.
