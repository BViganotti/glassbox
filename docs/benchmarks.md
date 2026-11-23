# Benchmarks

Every number in this file was printed by `./build/bench/glassbox_bench` on the
machine described below. Nothing is fabricated or extrapolated.

## Machine and build

- Apple Silicon (arm64), Darwin 25.6.0, 10 logical CPUs.
- Apple Clang 21, `-O3 -DNDEBUG`, libc++ hardening enabled.
- Trace files written to the system temp directory (SSD).
- Release build via `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`.

## Recorder

100,000 iterations, each emitting a log, a counter, an allocation and a free
(4 records per iteration), with an 8,192-slot ring and 1 MiB chunks:

```
Recorder append   399,386 records in 0.274 s · 1,455,388 ops/s · 20.9 MB/s · dropped=0
Recorder finish   0.008 s
```

The append path includes string interning, queue locking, writer-thread encoding
and file I/O. This is a workstation measurement, not a guarantee; expect lower
rates with allocation hooks enabled or a slow disk. The drop count is reported
per run and persisted in the trace when non-zero.

## Trace I/O and analysis

A synthetic trace with spans, allocations/frees, logs, samples and counters
(210,667 records, 3,255 KiB on disk), 5 runs, medians:

| Operation | Median |
|---|---:|
| Trace writer | 34.3 ms · 6.14M records/s |
| Trace reader (streaming) | 26.1 ms · 8.07M records/s |
| Trace memory | 47,104 KiB in memory |
| Timeline (200 buckets) | 3.7 ms |
| Heap reconstruction at T/2 | 7.4 ms |
| Flame aggregation | 3.6 ms |
| Moment diff (two heaps + analyses) | 20.0 ms |
| Query scan (limit 500) | 0.1 ms |

Notes:

- Reading now streams chunk-by-chunk from the file, so the loader's peak memory
  is the decoded model plus one payload (8 MiB max) rather than the whole file.
- Reading streams chunks from disk (peak memory is the decoded model plus one
  8 MiB payload) and is dominated by validation (CRC, bounds checks, string
  interning), not allocation.
- Heap reconstruction and diff are O(records) sweeps; they are fast enough for
  interactive scrubbing on traces of this size, but a maximum-size trace (2M
  records) will be proportionally slower.
- The trace memory figure reflects the in-memory analysis model (records +
  fields + strings + capacities), not the file size.
- Query scan is cheap because the predicate matched early; a predicate that
  matches nothing still scans the range.

## Reproducing

```sh
cmake --preset release
cmake --build --preset release
./build/bench/glassbox_bench
```

The benchmark prints the same table with the current machine's numbers. Use
`--quick` for a smaller trace. Run it on an idle machine and compare medians,
not single samples.
