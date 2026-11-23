# Examples

Both examples are ordinary C++ programs that link the `glassbox` library.

## `glassbox_demo`

A game-loop simulation used for the product tour. It records real allocations
(including deliberate leaks), spans, logs, counters, snapshots, a 120 ms
latency spike and stack samples, then opens the workbench on the trace.

```sh
./build/glassbox_demo
./build/glassbox_demo --frames 30 --out traces/short.gbt --no-browser
./build/glassbox_demo --no-serve --out examples/traces/demo.gbt
./build/glassbox_demo --mach-sampler
```

Things to find in the UI:

- **Heap at end** — three 32 KiB `Texture` allocations from
  `asset_loader.cpp:88` are still live, plus the scene graph.
- **Leak candidates** — `asset_loader.cpp:88` ranks first by bytes and age.
- **Flame graph** — `physics`, `animation`, `render` and `slow-frame` samples
  contain real symbols from this binary.
- **Spans at the spike** — scrub to roughly the midpoint: `SlowFrame` lasts
  about 120 ms and the `fps` counter drops to 14.5.
- **Diff 0 → end** — `inventory_count` drifts from 3 to 20 while
  `inventory_expected` stays at 3. That is the deliberate state bug.
- **Retained graph** — the `Scene` node retains its children and leaked
  textures.

The demo frees the intentionally leaked memory right after the recorder writes
the footer, so the process exits clean while the trace still shows the leak.

## `glassbox_basic`

The smallest embedding example: one span, one log pair, one tracked allocation,
a counter and a snapshot. It writes `traces/basic.gbt` and prints the record
count.

```sh
./build/glassbox_basic
./build/glassbox --trace traces/basic.gbt
```

## Writing your own

Start with [docs/recorder.md](../docs/recorder.md). The short version:

```cpp
gbx::Recorder recorder;
recorder.start("traces/app.gbt");
{
  auto span = recorder.begin_span("Work", "cpu");
  recorder.log(gbx::LogLevel::Info, "hello", "demo");
}
recorder.finish();
```
