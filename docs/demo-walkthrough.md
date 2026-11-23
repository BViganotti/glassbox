# Demo walkthrough

This is a guided tour of the committed demo trace
`examples/traces/demo.gbt`. To regenerate it:

```sh
./build/glassbox_demo --no-serve --out examples/traces/demo.gbt
```

The recording is 90 simulated frames with a real 120 ms stall at frame 45. It
contains a leak, a latency spike, a state drift, and a live ownership hierarchy.

## 1. Open it

```sh
./build/glassbox --trace examples/traces/demo.gbt
```

The workbench starts at the end of the trace, where the heap is at its final
state. The timeline shows two dense frame regions separated by the spike.

## 2. Inspect the live heap

Open **Heap**. At the end of the trace:

- live bytes are ~98 KiB across 18 objects;
- `asset_loader.cpp:88 · Texture` holds 96 KiB in 3 objects, age ~43 ms;
- `scene.cpp:41 · Scene` owns `scene.cpp:47 · SceneNode` and the textures;
- the treemap is dominated by Texture; select a site to filter objects;
- click an object to see its address, size, owner, allocation time and the
  originating `alloc` record.

Turn on **Retained graph** to see dominator-based retained sizes. The `Scene`
root retains its entire subtree; the treemap switches to per-object shapes.

## 3. Follow the latency spike

Scrub the timeline to just before the middle (or press `g s` for Spans and set
the scrubber around 63 ms). The `SlowFrame` span is about 120 ms long and sits
inside a `Frame` span. The overview's `fps` series dips to 14.5 at that moment.
Hovering the flame graph shows `slow-frame` samples.

## 4. Find the leak

Open **Leak candidates** on the overview or **Heap**:

| site | class | bytes | count |
|---|---|---|---|
| `asset_loader.cpp:88` | Texture | 96.0 KiB | 3 |
| `entities.cpp:18` | Entity | 2.0 KiB | 8 |
| `scene.cpp:47` | SceneNode | 192 B | 6 |
| `scene.cpp:41` | Scene | 32 B | 1 |

The ranking weights bytes by how long the allocation has been alive. The scene
graph and textures are intentionally alive at the end; the demo is an example,
not proof of a leak in arbitrary code.

## 5. Diff two moments

Set A to the start (`[` after scrubbing to 0, or the A button) and B to the end
(`]`). Open **Diff A/B**:

- heap grows by 98.2 KiB across 6 sites;
- `asset_loader.cpp:88` +96 KiB, `scene.cpp:41` +32 B, `scene.cpp:47` +192 B;
- counters move from 0 to their final values;
- `inventory_count` changes from 3 to 20 while `inventory_expected` stays 3 —
  the state bug;
- log counts by level include the three `asset_loader` errors.

The state table (State → "show A/B diff") shows the same change with before and
after values.

## 6. Query records

Press `/` and try:

```
type:alloc AND site ~ "asset_loader"
type:log AND level >= 3
msg ~ "inventory" AND ts > 60ms
```

The result table is clickable; each record opens in the inspector with all
fields and the raw JSON.

## 7. Record a live run

Click **Record demo**. The UI opens an SSE stream, shows a recording badge with
the event count, and reloads the trace when the run finishes. Use the form to
change frame count, enable the Mach sampler, or disable the real 120 ms sleep
for a faster run.

## Keyboard

- `space` play/pause, arrows step, shift+arrows step ten
- `[` / `]` set A/B
- `g` `o|f|h|s|l|v|d` switch views
- `/` query, `Esc` close overlays
- wheel zooms the timeline around the cursor; shift+drag selects a window
