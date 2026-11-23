#!/bin/sh
# Longer deterministic soak run. Not part of ctest; use before a release or
# after touching parsing/analysis code.
set -eu
BUILD=${1:-build}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/$BUILD/fuzz/glassbox_fuzz"
if [ ! -x "$BIN" ]; then echo "build first: make build" >&2; exit 1; fi
echo "soak: reader 2000"
"$BIN" reader 2000
echo "soak: query 20000"
"$BIN" query 20000
echo "soak: json 20000"
"$BIN" json 20000
echo "soak: http 300"
"$BIN" http 300
echo "soak: roundtrip 2000"
"$BIN" roundtrip 2000
echo "soak: demo loop"
OUT=$(mktemp -d)/soak.gbt
for i in 1 2 3 4 5; do
  "$ROOT/$BUILD/glassbox_demo" --no-serve --frames 90 --out "$OUT" >/dev/null 2>&1
  "$ROOT/$BUILD/glassbox_basic" >/dev/null 2>&1 || true
done
echo "soak: ok"
