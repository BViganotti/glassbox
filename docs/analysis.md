# Analysis

All analysis functions are pure over an immutable `Trace`. They are
deterministic: equal traces produce equal output, including ordering, and every
result is bounded by explicit caps.

## Span tree

`analyze_spans` walks sorted records once:

- A `span_begin` with a positive, unique `id` becomes a `SpanInfo` node.
  Duplicate ids and non-positive ids are ignored.
- A `span_end` closes the matching span at its timestamp. Unknown ids,
  duplicate ends and ends for already-closed spans increment
  `unmatched_ends`.
- Spans without an end remain open; their duration runs to the end of the
  trace. `unclosed` counts them.
- Parents that do not exist (or self-parents) become roots. Parent cycles are
  allowed; depth is computed with a hard 64-step guard, so malformed traces
  cannot hang the UI.
- Children lists are aligned with the span array; roots are listed separately.

`active_spans(trace, at)` returns spans with `begin <= at` and
`end > at` (or unclosed), ordered by begin time then id. Depth comes from the
parent chain with the same guard.

## Timeline

`build_timeline` divides a window into 1..512 buckets and aggregates:

- allocation bytes and counts, free counts;
- log counts and error counts (`level >= Error`);
- sample counts and span starts;
- span nanoseconds overlapping each bucket.

Span time is summed, not unioned: nested spans and concurrent threads can make
a bucket's span time exceed wall-clock time. That is intentional — the UI uses
it as an activity heat signal. The window is clamped and never zero-width.

## Counters and state

`counter_series` groups `counter` and `gauge` records by name, computes
min/max/last, and evenly downsamples long series while preserving the first and
last points. Name filtering is a case-insensitive substring. Series and points
are sorted deterministically.

`state_at` replays `snapshot` records up to a timestamp into a sorted map; the
last write for a key wins. Numeric and string values are both kept, along with
the timestamp of the last update. Snapshot records are allowed to carry any
number of keys; the recorder splits large snapshots across consecutive
records, and replay treats them as one logical write.

## Heap reconstruction

`heap_at` replays `alloc`/`free` records in timestamp order:

- A record with a positive pointer and non-negative size creates an allocation.
  Reusing a live pointer implicitly closes the previous allocation at the new
  timestamp (realloc-like behavior) and counts a free.
- A free for a live pointer closes it; a free for an unknown pointer increments
  `unknown_frees`.
- The replay tracks the running total and records the peak and its timestamp.

From the live set it computes total bytes, object count, and per-site
statistics (site + class key): live bytes/count, total allocation bytes/count,
frees, and average age. Sites and objects are sorted (bytes desc, then stable
tie-breakers) and capped (`max_sites`, `max_objects`), with the number omitted
reported as `sites_truncated` / `objects_truncated`.

`live_allocations` returns the same live set sorted by size, capped.

## Leak candidates

`leak_candidates` groups allocations still live at the end of the trace by site
and class. Each candidate receives

```
score = bytes * (1 + 4 * age_fraction)
```

where `age_fraction = (end - oldest_allocation) / trace_duration`. Sorting is
score desc, bytes desc, site, class. This ranks large, long-lived allocations
first; it is a heuristic, clearly labelled as "candidates", not proof of a
leak.

## Flame graphs

`build_flame` aggregates `sample` records in a window into a call tree:

- `frames` is split on `0x1f`; samples are root-first.
- Weights add to every node on the stack; leaves also add self time. A sample
  whose path exceeds the node or depth budget is marked truncated and its
  weight is charged to the deepest visited node, keeping totals consistent.
- Child ordering is total desc then name, so output is stable.
- When a window contains no samples, the result falls back to
  `build_span_flame`, flagged by `derived_from_spans`.

`build_span_flame` computes each span's clipped duration, subtracts its
children's clipped durations to obtain exclusive time, and adds that exclusive
time along the root-first span-name path into the same tree builder. Sibling
spans with the same name therefore merge into one frame.

Complexity is O(samples × frames) for samples and O(spans × depth) for the span
fallback, both bounded by the tree node cap.

## Heap ownership graph and dominators

`build_heap_graph` builds a directed graph whose nodes are the live allocations
at a time, with an edge `owner -> allocation` when the owner pointer is also a
live allocation. Allocations whose owner is absent or not live are roots. A
synthetic root connects all roots, and any node left unreachable by a cycle is
attached as an additional root so every node participates.

Retained sizes use the Cooper–Harvey–Kennedy iterative dominator algorithm:

1. Iterative DFS from the synthetic root, computing reverse postorder.
2. Intersect predecessor dominators in RPO until stable (bounded to 1,000
   iterations).
3. Accumulate sizes bottom-up over the dominator tree.

The result exposes `idom` (index or -1 for a root), dominator children, and
`retained` bytes per node. Node and edge caps (`max_nodes`, `max_edges`) mark
`truncated`; a non-converging graph reports `converged=false` rather than
presenting incorrect numbers. Both cases are surfaced in the UI.

## Diffs

`diff(trace, a, b)` computes two heap snapshots, span aggregates, cumulative
log counts, counter values and merged state:

- Heap: live bytes and counts per site/class; only entries that changed are
  emitted. Sorted by |bytes delta|, then site and class.
- Spans: cumulative counts and total durations for spans that began at or
  before each moment, grouped by name. Because child spans overlap parents,
  totals can exceed wall-clock time. Sorted by |duration delta| then name.
- Logs: cumulative counts by level; only levels with new records are emitted,
  sorted by level.
- Counters: last value at each moment; only changed or newly-present series
  are emitted, sorted by name.
- State: merged state before/after, only keys that changed or appeared,
  sorted by key.

Every list is capped by the `limit` argument. `b < a` is accepted (the deltas
are simply negative) so the UI can swap moments without a special case.
