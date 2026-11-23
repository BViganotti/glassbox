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
Recorder append   397,085 records in 0.278 s · 1,428,092 ops/s · 20.6 MB/s · dropped=0
Recorder finish   0.004 s
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
| Trace writer | 23.3 ms · 9.0M records/s |
| Trace reader | 25.1 ms · 8.4M records/s |
| Trace memory | 47,104 KiB in memory |
| Timeline (200 buckets) | 4.6 ms |
| Heap reconstruction at T/2 | 8.2 ms |
| Flame aggregation | 3.7 ms |
| Moment diff (two heaps + analyses) | 21.7 ms |
| Query scan (limit 500) | 0.1 ms |

Notes:

- Reading is dominated by validation (CRC, bounds checks, string interning), not
  allocation.
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
