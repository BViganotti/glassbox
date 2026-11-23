# Trace format (version 1)

Glassbox traces are portable binary files with the `.gbt` extension. The format
is designed for streaming writes, bounded decoding of untrusted input, and
clean recovery from a truncated tail. All integers are little-endian; varints
are unsigned LEB128 unless stated otherwise.

## File layout

```
"GLASSBOX"          8 bytes  magic
u32                 version (1)
u32                 flags (0, reserved)
u64                 wall_start_ns (Unix epoch)
u32                 pid
u32                 meta_count (5)
  repeated:
    u32 length
    bytes          app, app_version, exe_path, arch, os
chunk*             see below
footer             see below (absent when a writer was interrupted)
```

The reader accepts additional metadata strings (up to 16) for forward
compatibility and ignores unknown flags. An unknown version is rejected with
`Unsupported`.

## Chunks

```
"GBXCHNK1"          8 bytes
u32                 payload length (<= 8 MiB)
u32                 CRC-32 of payload
u32                 record count
payload
```

The payload holds its own string table so a writer can stream without seeking:

```
varint              string_count (<= 65536)
  repeated:
    varint length  (<= 64 KiB)
    bytes
varint              record_count (<= 1,000,000)
record*
```

### Records

Each record starts a new timestamp chain within its chunk:

```
record:
  timestamp:    varint (absolute, first record) or zig-zag varint (delta)
  u8            type (0..12)
  varint        thread slot
  varint        span id (0 = none)
  varint        field count (<= 64)
  field*:
    varint      string-table index of the key
    u8          value type
    value
```

Small backward timestamp steps are legal and encoded with a negative delta, so
multi-threaded producers do not need a global ordering lock. The reader
rejects underflow, overflow, timestamps beyond 2^63, unknown types, unknown
value tags, non-finite floats and out-of-range string indices. After loading,
records are stably sorted by timestamp (emission order breaks ties).

Value encodings:

| Tag | Name | Encoding |
|---:|---|---|
| 1 | int | zig-zag varint (also used for pointers and timestamps) |
| 2 | float | 8-byte IEEE-754 double, little-endian |
| 3 | string | varint string-table index |
| 4 | bool | one byte, 0 or 1 |

Pointers are encoded as the raw 64-bit value through an int64 round trip, so
addresses with the high bit set survive. The UI renders them as hexadecimal.

### Footer

```
"GBXEND01"          8 bytes
u64                 chunk count
u64                 record count
u64                 last timestamp (duration)
u32                 CRC-32 of the preceding 24 bytes
```

The footer is written last. Its counts must match the decoded content and its
duration may not precede the final record; otherwise the file is rejected as
corrupt.

## Record semantics

Records are generic key/value bags. These keys have defined meaning:

| Type | Meaning | Well-known fields |
|---:|---|---|
| 0 | meta | none (process metadata lives in the header) |
| 1 | thread name | `name` |
| 2 | span begin | `id`, `parent`, `name`, `kind` |
| 3 | span end | `id` |
| 4 | log | `level` (0..5), `msg`, optional `category` |
| 5 | counter | `name`, `value`, optional `unit` |
| 6 | gauge | `name`, `value` |
| 7 | alloc | `ptr`, `size`, `site`, optional `class`, optional `owner` |
| 8 | free | `ptr` |
| 9 | snapshot | arbitrary state keys; the key is the state variable |
| 10 | sample | `frames` (root-first, separated by `0x1f`), `weight`, optional `truncated` |
| 11 | marker | `label` (loss accounting markers use `dropped_records`, ...) |
| 12 | custom | application-defined |

The record's `span` field is the innermost span active on the emitting thread,
except that a span-begin record carries its own id and a span-end record
carries the parent id. Span ids are generated monotonically by the recorder and
need not be contiguous after filtering.

## Hard limits

| Limit | Value |
|---|---:|
| File size accepted by the reader | 2 GiB |
| Decoded in-memory budget | 1 GiB (configurable) |
| Records per trace | 8,000,000 |
| Fields per trace | 32,000,000 |
| Fields per record | 64 |
| Strings per trace | 524,288 |
| String bytes per trace | 128 MiB |
| String length | 64 KiB |
| Chunk payload | 8 MiB |
| Chunks per trace | 65,536 |
| Thread slots | 65,536 |

Limits are enforced at construction and decoding time; crossing one yields
`LimitExceeded` instead of unbounded allocation. Readers never trust lengths
and always check them against the remaining bytes before allocating.

## Truncation and corruption

Two different failures are distinguished:

- **Truncated**: the file ends inside a chunk header, payload or the footer.
  With `LoadOptions::recover_truncated = true` (the default) the complete
  prefix is loaded and `TraceDiagnostics` records `truncated`,
  `recovered` and a warning string. The UI shows the warning.
- **Corrupt**: checksum mismatch, bad magic, impossible lengths, trailing
  bytes after the footer, or a footer that disagrees with the decoded data.
  Loading fails with `Corrupt`; damaged bytes are never silently ignored.

A trace written by a crashed recorder has no footer, so it loads with a
truncation warning rather than a hard failure. `recover_truncated = false`
turns every incomplete tail into an error, which is useful for validation
tooling.

## Random access

Chunks are bounded in size, so callers can index them without loading the whole
file. The in-memory `Trace` records each chunk's file offset and timestamp
range in `Trace::chunks`, and `Trace::record_range(begin, end)` performs binary
searches over the sorted record array. `load_trace` streams chunks from disk: only one payload (at most 8 MiB) is held
in addition to the decoded model. The decoded model itself is bounded by
`TraceLimits::max_memory_bytes` (1 GiB by default), so a large file fails
cleanly with `LimitExceeded` instead of risking the process. `load_trace_bytes`
remains available for in-memory/fuzzing use.

## Golden corpus

`examples/traces/demo.gbt` is a committed trace from the demo workload. The
`golden_demo_trace_loads_and_analyses` test guards both the format and the
analysis pipeline. `tests/test_trace_io.cpp` contains mutation and truncation
cases; `fuzz/fuzz_main.cpp` mode `reader` continuously mutates the demo trace.
