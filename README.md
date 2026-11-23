# Glassbox

Glassbox is a time-travel inspector for native C++ applications. It records CPU
samples, allocations, spans, logs, counters and state snapshots into a portable
trace file, then lets you scrub through the recording in a local workbench:
inspect the heap at any instant, walk the span tree, read a flame graph, find
leaks and diff two moments.

Glassbox runs entirely on your machine. The UI is served on `127.0.0.1` from an
embedded HTTP server; the HTML, CSS and JavaScript are compiled into the binary.
There are no third-party dependencies and no network access at runtime.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires macOS with Apple Clang, CMake 3.20+ and Ninja.

## Run

```sh
./build/glassbox                      # opens the newest trace, or records the demo
./build/glassbox path/to/file.gbt     # opens a trace
./build/glassbox_demo                 # records the demo workload, then opens the UI
./build/glassbox --no-browser         # serve without launching a browser
```

The launcher prints nothing you must interact with: it starts the server on an
ephemeral loopback port and opens your default browser at a URL carrying a
one-time session token.

## Using the library

```cpp
#include <glassbox/glassbox.hpp>

gbx::Recorder rec;
rec.start("traces/session.gbt");

{
  auto span = rec.begin_span("ParseShot");
  rec.log(gbx::LogLevel::Info, "parsing", "input");
  rec.alloc(ptr, 4096, "decode.cpp:88", "Image");
  rec.counter("fps", 59.8);
}
rec.finish();
```

See `docs/` for the trace format, recorder lifetime rules, analysis algorithms
and the query language.

## Contents

- `include/glassbox/` — public, documented C++20 API.
- `src/trace/` — binary trace format, CRC, varints, reader/writer.
- `src/recorder/` — bounded multi-producer recorder and span API.
- `src/alloc/` — allocation tracking, object registry and opt-in hooks.
- `src/sample/` — manual and Mach-based sampling with symbolization.
- `src/analysis/` — timelines, flame graphs, heap graphs, dominators, diffs.
- `src/query/` — predicate language for record filtering.
- `src/ui/` — loopback HTTP/SSE server, JSON, embedded assets.
- `examples/` — demo workload with a real leak and a latency spike.
- `tests/`, `fuzz/`, `bench/` — verification and measurement.

## License

MIT. See [LICENSE](LICENSE).
