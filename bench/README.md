# Benchmarks

`glassbox_bench` measures the real code paths and prints only runtime-measured
numbers:

- recorder append throughput (log + counter + alloc + free per iteration),
  finish latency, bytes/s and drop accounting;
- writer and reader throughput over a synthetic 210k-record trace, plus the
  in-memory analysis footprint;
- median timeline, heap reconstruction, flame aggregation, diff and query
  times over five runs.

```sh
./build/bench/glassbox_bench
./build/bench/glassbox_bench --quick
```

The trace files are written under the system temp directory and are not
cleaned by the process; remove `glassbox-bench-*` directories if needed.

Results are environment-dependent. The numbers recorded for this repository
and the machine/methodology are in [docs/benchmarks.md](../docs/benchmarks.md).
