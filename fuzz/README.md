# Fuzz harnesses

`glassbox_fuzz` is a deterministic, seed-driven fuzzer. It intentionally does
not depend on libFuzzer or any other external framework, in keeping with the
project's no-third-party-dependency rule.

```sh
./build/fuzz/glassbox_fuzz <mode> [iterations]
```

| Mode | What it feeds | Invariants checked |
|---|---|---|
| `reader` | demo trace mutated: bit flips, truncation, insertions, deletions, random bytes | successful loads are finalized/sorted, field and string indices are in range; every analysis pass runs on the result |
| `query` | random predicates and mutations of valid seeds | parsing never throws; valid queries evaluate over a real trace; limits hold |
| `json` | random bytes and mutated valid JSON | parser never throws; only bounded values are accepted |
| `http` | random bytes and near-valid requests over loopback | server survives; still answers a valid request afterwards |
| `roundtrip` | random in-memory traces with varying chunk thresholds | load succeeds, record counts match, invariants hold |

ctest runs each mode with a fixed iteration count and a 180 s timeout. Increase
iterations locally for longer runs; the seed is fixed, so failures reproduce.

The readers/parsers are also exercised by targeted malformed-input tests in
`tests/`, including limits that are easy to hit deterministically. Fuzzing is a
complement, not a replacement for those tests.
