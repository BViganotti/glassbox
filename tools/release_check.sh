#!/bin/sh
# Local release verification. No CI is used by this project, so this script is
# the reproducible checklist. Set SKIP_LINUX=1 / SKIP_SMOKE=1 to shorten it.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-build}
cd "$ROOT"

echo "== configure/build =="
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j 8

echo "== tests =="
ctest --test-dir "$BUILD" --output-on-failure

echo "== benchmarks (quick) =="
"$ROOT/$BUILD/bench/glassbox_bench" --quick

if [ "${SKIP_SMOKE:-0}" != "1" ] && \
   [ -x "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" ]; then
  echo "== UI smoke =="
  "$ROOT/tools/ui_smoke.sh" "$BUILD"
fi

if [ "$(uname -s)" = "Darwin" ]; then
  echo "== app bundle =="
  "$ROOT/tools/make_bundle.sh" "$BUILD" dist
  "$ROOT/dist/Glassbox.app/Contents/MacOS/Glassbox" --version
fi

if [ "${SKIP_LINUX:-0}" != "1" ] && command -v docker >/dev/null 2>&1; then
  echo "== linux build/test =="
  "$ROOT/tools/linux_test.sh"
fi

echo "release check: ok"
