#!/bin/sh
# Builds and tests the library and binaries on Linux via Docker. The source is
# mounted read-only-ish and all artifacts stay inside the container.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${GBX_LINUX_IMAGE:-glassbox-linux-builder}
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -f "$ROOT/tools/Dockerfile.linux" -t "$IMAGE" "$ROOT/tools"
fi
exec docker run --rm -v "$ROOT":/src -w /src "$IMAGE" sh -c '
  set -e
  cmake -S . -B /tmp/build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/build-linux -j "$(nproc)"
  ctest --test-dir /tmp/build-linux --output-on-failure
'
