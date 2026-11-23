# Testing, fuzzing and benchmarks

Everything here runs locally with CMake and Ninja. There are no CI
configurations in the repository by design.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

test names:

| Test | What it covers |
|---|---|
| `unit` | 120+ tests: codecs, CRC, string table, trace model/IO, recorder, hooks, allocation tracking, sampling, spans, timeline, series/state, heap, leaks, flame, dominators, diff, query, JSON, HTTP, workbench, demo |
| `hooks` | dedicated binary linking `glassbox_hooks` (global new/delete interception) |
| `fuzz_reader` | mutated/truncated traces decoded and analyzed |
| `fuzz_query` | mutated/random predicates parsed and evaluated |
| `fuzz_json` | malformed JSON bodies parsed |
| `fuzz_http` | random HTTP requests against a live loopback server |
| `fuzz_roundtrip` | random traces saved/loaded with varying chunk sizes |

The unit test binary supports a substring filter: `./build/tests/glassbox_tests heap`.

Tests use temporary directories and clean up; the committed golden trace
(`examples/traces/demo.gbt`) is never modified.

## Sanitizers

ASan + UBSan:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGBX_SANITIZER=address,undefined
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

TSan:

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGBX_SANITIZER=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

Notes:

- The allocation-hooks test is skipped in sanitizer builds: ASan/TSan install
  their own `operator new/delete`, and replacing them is documented as
  unsupported.
- The Mach sampler reads other threads' registers and stack memory; those
  reads are not instrumented, but all bookkeeping is atomic/mutex-protected.
- LeakSanitizer is fine with the demo: objects that the trace shows as leaked
  are deleted immediately after the recorder writes the footer.

The exact commands and results used for the release report are recorded in
`FINAL_REPORT.md`.

## Fuzzing

The fuzz binary is deterministic and seed-driven, not libFuzzer-based (no
third-party dependencies). Modes:

```sh
./build/fuzz/glassbox_fuzz reader 500      # mutated golden/demo traces
./build/fuzz/glassbox_fuzz query 2000      # predicates
./build/fuzz/glassbox_fuzz json 2000       # request bodies
./build/fuzz/glassbox_fuzz http 200        # raw HTTP requests
./build/fuzz/glassbox_fuzz roundtrip 500   # writer/reader property test
```

The reader mode accepts malformed input only through `Status` (through both the
in-memory and streaming file loaders), then validates
invariants on accepted traces (sorted records, field bounds, string ids) and
runs every analysis pass on them. HTTP mode checks that the server still
answers a valid request after a stream of malformed ones.

## Benchmarks

```sh
./build/bench/glassbox_bench          # full run
./build/bench/glassbox_bench --quick  # smaller trace
```

It prints measured recorder append/finish rates, writer and reader throughput,
trace memory, and median times for timeline, heap reconstruction, flame
aggregation, diff and query. The file paths go to a temp directory. No results
are hard-coded; run it on your machine to compare.

## Test design

- Every limit has a test that crosses it: oversized records, strings, chunks,
  bodies, headers, query depth, graph nodes/edges, flame nodes.
- Malformed traces have dedicated tests for bad magic, checksum mismatch,
  unknown version, trailing garbage, empty files and truncated chunks with and
  without recovery.
- Threading is covered by multi-producer recorder tests and by the workbench
  tests running real sockets and SSE streams.
- Golden traces protect the format and analysis; generated demo runs check
  that the workload still contains a leak, a spike and state drift.
